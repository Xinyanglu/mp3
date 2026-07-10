/*
 * LCD and UI task handling.
 *
 * Owns LVGL display setup, Bluetooth discovery list rendering, and button-driven
 * screen navigation.
 */
#include "screen.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "bt_app.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_bt_defs.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdcard.h"
#include "player.h"

#define LCD_HOST SPI2_HOST

#define LCD_H_RES 320
#define LCD_V_RES 240
#define LCD_DRAW_BUFF_HEIGHT 20

#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define LCD_CMD_BITS 8
#define LCD_PARAM_BITS 8
#define LCD_BITS_PER_PIXEL 16

#define LCD_BK_LIGHT_ON_LEVEL 1
#define LCD_BK_LIGHT_OFF_LEVEL !LCD_BK_LIGHT_ON_LEVEL

#define PIN_NUM_MOSI GPIO_NUM_33
#define PIN_NUM_PCLK GPIO_NUM_25
#define PIN_NUM_CS GPIO_NUM_26
#define PIN_NUM_DC GPIO_NUM_27
#define PIN_NUM_RST GPIO_NUM_14
#define PIN_NUM_BK_LIGHT GPIO_NUM_13

#define MAX_BT_DEVICE_NUM 16
#define MAX_BT_DEVICE_TIMEOUT_MS 10000

typedef struct {
    char name[MAX_DEVICE_NAME_LEN];
    esp_bd_addr_t bda;
    int64_t last_discovered_ms;
} screen_bt_device_t;


static const char* TAG = "screen";

static esp_lcd_panel_io_handle_t lcd_io;
static esp_lcd_panel_handle_t lcd_panel;
static lv_display_t* display;
static screen_bt_device_t bt_devices[MAX_BT_DEVICE_NUM];
static uint8_t num_bt_devices;
static int selected_bt_device_idx = -1;
static int selected_song_idx      = -1;
static esp_timer_handle_t bt_scan_refresh_timer;
static screen_state current_screen = SCREEN_STATE_BT_DISCOVERY;
static lv_obj_t* song_progress_bar;
static lv_obj_t* song_progress_time_label;

static esp_err_t init_lcd(void);
static esp_err_t init_lvgl(void);
static void add_bt_device(const char* name, esp_bd_addr_t bda);
static int find_bt_device(esp_bd_addr_t bda);
static void screen_refresh_bt_scan(void* arg);
static void screen_show_bt_scan(void);
static esp_err_t screen_add_bt_device(const char* name, esp_bd_addr_t bda);
static esp_err_t screen_handle_btn_press(screen_button button);
static esp_err_t screen_handle_bt_discovery_btn_press(screen_button button);
static esp_err_t screen_handle_song_select_btn_press(screen_button button);
static const char* screen_button_to_str(screen_button button);
static int find_first_bt_device(void);
static int find_next_bt_device(int start_idx);
static int find_prev_bt_device(int start_idx);
static void screen_show_song_selection(void);
static void screen_show_song_loading(void);
static void screen_show_song_playing(void);
static void screen_update_song_progress(uint32_t elapsed_seconds, uint32_t total_seconds);
static void screen_format_time(char* buf, size_t buf_size, uint32_t seconds);

static QueueHandle_t screen_event_queue = NULL;
static TaskHandle_t screen_task_handle  = NULL;

static esp_err_t init_lcd(void) {
    const gpio_config_t bk_gpio_config = {
        .mode         = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_NUM_BK_LIGHT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bk_gpio_config), TAG, "Backlight GPIO config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_NUM_BK_LIGHT, LCD_BK_LIGHT_OFF_LEVEL), TAG, "Backlight off failed");

    const spi_bus_config_t buscfg = {
        .sclk_io_num     = PIN_NUM_PCLK,
        .mosi_io_num     = PIN_NUM_MOSI,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_BUFF_HEIGHT * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "SPI bus init failed");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num       = PIN_NUM_DC,
        .cs_gpio_num       = PIN_NUM_CS,
        .pclk_hz           = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits      = LCD_CMD_BITS,
        .lcd_param_bits    = LCD_PARAM_BITS,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &lcd_io), TAG, "Panel IO init failed");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(lcd_io, &panel_config, &lcd_panel), TAG, "Panel init failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(lcd_panel), TAG, "Panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(lcd_panel), TAG, "Panel start failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(lcd_panel, true), TAG, "Panel invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(lcd_panel, true), TAG, "Panel enable failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_NUM_BK_LIGHT, LCD_BK_LIGHT_ON_LEVEL), TAG, "Backlight on failed");

    return ESP_OK;
}

static esp_err_t init_lvgl(void) {
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL init failed");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = lcd_io,
        .panel_handle  = lcd_panel,
        .buffer_size   = LCD_H_RES * LCD_DRAW_BUFF_HEIGHT,
        .double_buffer = true,
        .hres          = LCD_H_RES,
        .vres          = LCD_V_RES,
        .monochrome    = false,
#if LVGL_VERSION_MAJOR >= 9
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif
        .rotation =
            {
                .swap_xy  = true,
                .mirror_x = true,
                .mirror_y = false,
            },
        .flags =
            {
                .buff_dma = true,
#if LVGL_VERSION_MAJOR >= 9
                .swap_bytes = true,
#endif
            },
    };
    display = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(display != NULL, ESP_FAIL, TAG, "LVGL display registration failed");

    return ESP_OK;
}

static void screen_task_handler(void* arg __attribute__((unused))) {
    screen_msg msg;
    while (1) {
        if (pdTRUE == xQueueReceive(screen_event_queue, &msg, (TickType_t)portMAX_DELAY)) {
            ESP_LOGD(TAG, "%s, event: 0x%x", __func__, msg.event);

            switch (msg.event) {
            case SCREEN_EVT_REFRESH_BT_SCAN:
                screen_refresh_bt_scan(NULL);
                break;

            case SCREEN_EVT_BTN_PRESS:
                ESP_LOGI(TAG, "Button press: %s (%d)", screen_button_to_str(msg.button), msg.button);
                if (current_screen == msg.screen_state) {
                    screen_handle_btn_press(msg.button);
                }
                break;

            case SCREEN_EVT_BT_DEVICE_FOUND:
                screen_add_bt_device(msg.device_name, msg.bda);
                break;

            case SCREEN_EVT_BT_DEVICE_CONNECTED:
                current_screen = SCREEN_STATE_SONG_SELECT;
                screen_show_song_selection();
                break;

            case SCREEN_EVT_SONG_PLAYING:
                current_screen = SCREEN_STATE_SONG_PLAYING;
                screen_show_song_playing();
                break;

            case SCREEN_EVT_SONG_PROGRESS:
                screen_update_song_progress(msg.elapsed_seconds, msg.total_seconds);
                break;

            default:
                ESP_LOGW(TAG, "%s, unhandled event: %d", __func__, msg.event);
                break;
            }
        }
    }
}

esp_err_t screen_init(void) {
    ESP_RETURN_ON_ERROR(init_lcd(), TAG, "LCD init failed");
    ESP_RETURN_ON_ERROR(init_lvgl(), TAG, "LVGL setup failed");

    screen_event_queue = xQueueCreate(10, sizeof(screen_msg));
    xTaskCreate(screen_task_handler, "ScreenTask", 3072, NULL, 10, &screen_task_handle);

    const esp_timer_create_args_t timer_args = {
        .callback = screen_notify_bt_refresh,
        .name     = "bt_scan_refresh",
    };

    ESP_RETURN_ON_ERROR(
        esp_timer_create(&timer_args, &bt_scan_refresh_timer), TAG, "BT scan refresh timer create failed");

    ESP_RETURN_ON_ERROR(
        esp_timer_start_periodic(bt_scan_refresh_timer, 5 * 1000 * 1000), TAG, "BT scan refresh timer start failed");

    return ESP_OK;
}

static const char* screen_button_to_str(screen_button button) {
    static const char* const button_names[] = {
        [SCREEN_BTN_UP]     = "SCREEN_BTN_UP",
        [SCREEN_BTN_DOWN]   = "SCREEN_BTN_DOWN",
        [SCREEN_BTN_LEFT]   = "SCREEN_BTN_LEFT",
        [SCREEN_BTN_RIGHT]  = "SCREEN_BTN_RIGHT",
        [SCREEN_BTN_SELECT] = "SCREEN_BTN_SELECT",
    };

    if (button >= SCREEN_BTN_MAX || button_names[button] == NULL) {
        return "UNKNOWN";
    }

    return button_names[button];
}

void screen_notify_bt_device_found(const char* device_name, esp_bd_addr_t bda) {
    screen_msg msg = {.event = SCREEN_EVT_BT_DEVICE_FOUND};
    strncpy(msg.device_name, device_name, MAX_DEVICE_NAME_LEN);
    memcpy(msg.bda, bda, sizeof(esp_bd_addr_t));
    xQueueSend(screen_event_queue, &msg, 0);
}

void screen_notify_bt_refresh(void* arg) {
    (void)arg;
    screen_msg msg = {.event = SCREEN_EVT_REFRESH_BT_SCAN, .device_name = "\0"};
    xQueueSend(screen_event_queue, &msg, 0);
}

void screen_notify_button_press(screen_button button) {
    screen_msg msg = {.event = SCREEN_EVT_BTN_PRESS, .button = button, .screen_state = current_screen};
    xQueueSend(screen_event_queue, &msg, 0);
}

void screen_notify_show_song_selection(void) {
    screen_msg msg = {.event = SCREEN_EVT_BT_DEVICE_CONNECTED};
    xQueueSend(screen_event_queue, &msg, 0);
}

void screen_notify_show_song_playing(void) {
    screen_msg msg = {.event = SCREEN_EVT_SONG_PLAYING};
    xQueueSend(screen_event_queue, &msg, 0);
}

void screen_notify_song_progress(uint32_t elapsed_seconds, uint32_t total_seconds) {
    screen_msg msg = {
        .event = SCREEN_EVT_SONG_PROGRESS,
        .elapsed_seconds = elapsed_seconds,
        .total_seconds = total_seconds,
    };
    xQueueSend(screen_event_queue, &msg, 0);
}

static void screen_remove_old_bt_devices(void) {
    int64_t now = esp_timer_get_time() / 1000;
    for (int i = 0; i < MAX_BT_DEVICE_NUM; i++) {
        if (bt_devices[i].name[0] == '\0')
            continue;
        if (bt_devices[i].last_discovered_ms < (now - MAX_BT_DEVICE_TIMEOUT_MS)) {
            memset(&bt_devices[i], 0, sizeof(screen_bt_device_t));
            num_bt_devices--;
            if (selected_bt_device_idx == i) {
                selected_bt_device_idx = find_first_bt_device();
            }
        }
    }
}

static void add_bt_device(const char* name, esp_bd_addr_t bda) {
    for (int i = 0; i < MAX_BT_DEVICE_NUM; i++) {
        if (bt_devices[i].name[0] == '\0') {
            strncpy(bt_devices[i].name, name, MAX_DEVICE_NAME_LEN);
            memcpy(bt_devices[i].bda, bda, sizeof(bt_devices[i].bda));
            bt_devices[i].last_discovered_ms = esp_timer_get_time() / 1000;
            num_bt_devices++;
            if (selected_bt_device_idx < 0) {
                selected_bt_device_idx = i;
            }
            break;
        }
    }
}

static int find_bt_device(esp_bd_addr_t bda) {
    for (int i = 0; i < MAX_BT_DEVICE_NUM; i++) {
        if (bt_devices[i].name[0] == '\0') {
            continue;
        }
        if (memcmp(bt_devices[i].bda, bda, ESP_BD_ADDR_LEN) == 0)
            return i;
    }
    return -1;
}

static int find_first_bt_device(void) {
    for (int i = 0; i < MAX_BT_DEVICE_NUM; i++) {
        if (bt_devices[i].name[0] != '\0') {
            return i;
        }
    }

    return -1;
}

static int find_next_bt_device(int start_idx) {
    if (num_bt_devices == 0) {
        return -1;
    }

    for (int offset = 1; offset <= MAX_BT_DEVICE_NUM; offset++) {
        int idx = (start_idx + offset) % MAX_BT_DEVICE_NUM;
        if (bt_devices[idx].name[0] != '\0') {
            return idx;
        }
    }

    return -1;
}

static int find_prev_bt_device(int start_idx) {
    if (num_bt_devices == 0) {
        return -1;
    }

    for (int offset = 1; offset <= MAX_BT_DEVICE_NUM; offset++) {
        int idx = (start_idx - offset + MAX_BT_DEVICE_NUM) % MAX_BT_DEVICE_NUM;
        if (bt_devices[idx].name[0] != '\0') {
            return idx;
        }
    }

    return -1;
}

static void screen_refresh_bt_scan(void* arg) {
    (void)arg;
    screen_remove_old_bt_devices();
    screen_show_bt_scan();
}

static esp_err_t screen_add_bt_device(const char* name, esp_bd_addr_t bda) {
    screen_remove_old_bt_devices();
    if (name == NULL)
        return ESP_ERR_INVALID_ARG;

    if (name[0] == '\0' || strnlen(name, MAX_DEVICE_NAME_LEN) >= MAX_DEVICE_NAME_LEN) {
        ESP_LOGE(TAG, "Device name too long");
        return ESP_ERR_INVALID_SIZE;
    }
    int idx = find_bt_device(bda);
    if (idx >= 0)
        bt_devices[idx].last_discovered_ms = esp_timer_get_time() / 1000;
    else if (num_bt_devices < MAX_BT_DEVICE_NUM)
        add_bt_device(name, bda);

    return ESP_OK;
}

static esp_err_t screen_handle_btn_press(screen_button button) {
    switch (current_screen) {
    case SCREEN_STATE_BT_DISCOVERY:
        return screen_handle_bt_discovery_btn_press(button);
    case SCREEN_STATE_SONG_SELECT:
        return screen_handle_song_select_btn_press(button);
    case SCREEN_STATE_SONG_LOADING:
    case SCREEN_STATE_SONG_PLAYING:
    default:
        return ESP_OK;
    }
}

static esp_err_t screen_handle_bt_discovery_btn_press(screen_button button) {
    if (selected_bt_device_idx < 0 || bt_devices[selected_bt_device_idx].name[0] == '\0') {
        selected_bt_device_idx = find_first_bt_device();
    }

    switch (button) {
    case SCREEN_BTN_UP:
        selected_bt_device_idx = find_prev_bt_device(selected_bt_device_idx);
        screen_show_bt_scan();
        break;
    case SCREEN_BTN_DOWN:
        selected_bt_device_idx = find_next_bt_device(selected_bt_device_idx);
        screen_show_bt_scan();
        break;
    case SCREEN_BTN_SELECT:
        if (selected_bt_device_idx >= 0)
            bt_app_connect_to(bt_devices[selected_bt_device_idx].name, bt_devices[selected_bt_device_idx].bda);
        break;
    default:
        break;
    }

    return ESP_OK;
}

static esp_err_t screen_handle_song_select_btn_press(screen_button button) {
    size_t songs_count = sdcard_get_song_count();

    if (songs_count == 0) {
        selected_song_idx = -1;
        return ESP_OK;
    }

    if (selected_song_idx < 0 || selected_song_idx >= (int)songs_count) {
        selected_song_idx = 0;
    }

    switch (button) {
    case SCREEN_BTN_UP:
        selected_song_idx = (selected_song_idx - 1 + (int)songs_count) % (int)songs_count;
        screen_show_song_selection();
        break;
    case SCREEN_BTN_DOWN:
        selected_song_idx = (selected_song_idx + 1) % (int)songs_count;
        screen_show_song_selection();
        break;
    case SCREEN_BTN_SELECT:
        current_screen = SCREEN_STATE_SONG_LOADING;
        screen_show_song_loading();
        ESP_RETURN_ON_ERROR(player_play(selected_song_idx), TAG, "Failed to start player");
        break;
    default:
        break;
    }

    return ESP_OK;
}

static void screen_show_bt_scan(void) {
    if (current_screen != SCREEN_STATE_BT_DISCOVERY) {
        return;
    }
    if (lvgl_port_lock(0)) {
        lv_obj_t* screen = lv_screen_active();
        lv_obj_clean(screen);
        song_progress_bar = NULL;
        song_progress_time_label = NULL;
        lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t* title = lv_label_create(screen);
        lv_label_set_text_fmt(title, "Discovered devices: %u", num_bt_devices);
        lv_obj_set_style_text_color(title, lv_color_hex(0x202020), LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 12);

        int y             = 40;
        bool found_device = false;
        for (int i = 0; i < MAX_BT_DEVICE_NUM; i++) {
            if (bt_devices[i].name[0] == '\0') {
                continue;
            }

            found_device           = true;
            lv_obj_t* device_label = lv_label_create(screen);
            lv_obj_set_width(device_label, LCD_H_RES - 24);
            lv_label_set_long_mode(device_label, LV_LABEL_LONG_WRAP);
            lv_label_set_text_fmt(device_label,
                                  "%s\n%02X:%02X:%02X:%02X:%02X:%02X",
                                  bt_devices[i].name,
                                  bt_devices[i].bda[0],
                                  bt_devices[i].bda[1],
                                  bt_devices[i].bda[2],
                                  bt_devices[i].bda[3],
                                  bt_devices[i].bda[4],
                                  bt_devices[i].bda[5]);
            if (i == selected_bt_device_idx) {
                lv_obj_set_style_bg_color(device_label, lv_color_hex(0x202020), LV_PART_MAIN);
                lv_obj_set_style_bg_opa(device_label, LV_OPA_COVER, LV_PART_MAIN);
                lv_obj_set_style_text_color(device_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            } else {
                lv_obj_set_style_text_color(device_label, lv_color_hex(0x202020), LV_PART_MAIN);
            }
            lv_obj_align(device_label, LV_ALIGN_TOP_LEFT, 12, y);
            y += 44;
        }

        if (!found_device) {
            lv_obj_t* status = lv_label_create(screen);
            lv_label_set_text(status, "Scanning...");
            lv_obj_set_style_text_color(status, lv_color_hex(0x666666), LV_PART_MAIN);
            lv_obj_align(status, LV_ALIGN_TOP_LEFT, 12, y);
        }

        lvgl_port_unlock();
    }
}

static void screen_show_song_selection(void) {
    size_t songs_count = sdcard_get_song_count();

    if (songs_count == 0) {
        selected_song_idx = -1;
    } else if (selected_song_idx < 0 || selected_song_idx >= (int)songs_count) {
        selected_song_idx = 0;
    }

    if (lvgl_port_lock(0)) {
        lv_obj_t* screen = lv_screen_active();
        lv_obj_clean(screen);
        song_progress_bar = NULL;
        song_progress_time_label = NULL;
        lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t* title = lv_label_create(screen);
        lv_label_set_text_fmt(title, "Songs: %u", (unsigned int)songs_count);
        lv_obj_set_style_text_color(title, lv_color_hex(0x202020), LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 12);

        int y = 40;
        for (size_t i = 0; i < songs_count; i++) {
            const sdcard_song_t* song = sdcard_get_song(i);
            if (song == NULL) {
                continue;
            }

            lv_obj_t* song_label = lv_label_create(screen);
            lv_obj_set_width(song_label, LCD_H_RES - 24);
            lv_label_set_long_mode(song_label, LV_LABEL_LONG_DOT);
            lv_label_set_text(song_label, song->name);

            if ((int)i == selected_song_idx) {
                lv_obj_set_style_bg_color(song_label, lv_color_hex(0x202020), LV_PART_MAIN);
                lv_obj_set_style_bg_opa(song_label, LV_OPA_COVER, LV_PART_MAIN);
                lv_obj_set_style_text_color(song_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            } else {
                lv_obj_set_style_text_color(song_label, lv_color_hex(0x202020), LV_PART_MAIN);
            }

            lv_obj_align(song_label, LV_ALIGN_TOP_LEFT, 12, y);
            y += 28;
        }

        if (songs_count == 0) {
            lv_obj_t* status = lv_label_create(screen);
            lv_label_set_text(status, "No songs found");
            lv_obj_set_style_text_color(status, lv_color_hex(0x666666), LV_PART_MAIN);
            lv_obj_align(status, LV_ALIGN_TOP_LEFT, 12, y);
        }

        lvgl_port_unlock();
    }
}

static void screen_show_song_loading(void) {
    const sdcard_song_t* song = NULL;

    if (selected_song_idx >= 0) {
        song = sdcard_get_song((size_t)selected_song_idx);
    }

    if (lvgl_port_lock(0)) {
        lv_obj_t* screen = lv_screen_active();
        lv_obj_clean(screen);
        song_progress_bar = NULL;
        song_progress_time_label = NULL;
        lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t* title = lv_label_create(screen);
        lv_label_set_text(title, "Loading song...");
        lv_obj_set_style_text_color(title, lv_color_hex(0x202020), LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 12);

        lv_obj_t* song_label = lv_label_create(screen);
        lv_obj_set_width(song_label, LCD_H_RES - 24);
        lv_label_set_long_mode(song_label, LV_LABEL_LONG_DOT);
        lv_label_set_text(song_label, song != NULL ? song->name : "Selected song");
        lv_obj_set_style_text_color(song_label, lv_color_hex(0x666666), LV_PART_MAIN);
        lv_obj_align(song_label, LV_ALIGN_TOP_LEFT, 12, 44);

        lvgl_port_unlock();
    }
}

static void screen_show_song_playing(void) {
    const sdcard_song_t* song = NULL;

    if (selected_song_idx >= 0) {
        song = sdcard_get_song((size_t)selected_song_idx);
    }

    if (lvgl_port_lock(0)) {
        lv_obj_t* screen = lv_screen_active();
        lv_obj_clean(screen);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t* title = lv_label_create(screen);
        lv_label_set_text(title, "Now playing");
        lv_obj_set_style_text_color(title, lv_color_hex(0x202020), LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 12);

        lv_obj_t* song_label = lv_label_create(screen);
        lv_obj_set_width(song_label, LCD_H_RES - 24);
        lv_label_set_long_mode(song_label, LV_LABEL_LONG_DOT);
        lv_label_set_text(song_label, song != NULL ? song->name : "Selected song");
        lv_obj_set_style_text_color(song_label, lv_color_hex(0x202020), LV_PART_MAIN);
        lv_obj_align(song_label, LV_ALIGN_TOP_LEFT, 12, 44);

        song_progress_bar = lv_bar_create(screen);
        lv_obj_set_size(song_progress_bar, LCD_H_RES - 24, 16);
        lv_bar_set_range(song_progress_bar, 0, 100);
        lv_bar_set_value(song_progress_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(song_progress_bar, lv_color_hex(0xE0E0E0), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(song_progress_bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(song_progress_bar, lv_color_hex(0x202020), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(song_progress_bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_align(song_progress_bar, LV_ALIGN_TOP_LEFT, 12, 84);

        song_progress_time_label = lv_label_create(screen);
        lv_label_set_text(song_progress_time_label, "0:00 / --:--");
        lv_obj_set_style_text_color(song_progress_time_label, lv_color_hex(0x666666), LV_PART_MAIN);
        lv_obj_align(song_progress_time_label, LV_ALIGN_TOP_LEFT, 12, 108);

        lvgl_port_unlock();
    }
}

static void screen_update_song_progress(uint32_t elapsed_seconds, uint32_t total_seconds) {
    char elapsed[12];
    char total[12];
    char progress_text[32];
    int progress = 0;

    if (current_screen != SCREEN_STATE_SONG_PLAYING) {
        return;
    }

    if (total_seconds > 0) {
        if (elapsed_seconds > total_seconds) {
            elapsed_seconds = total_seconds;
        }
        progress = (int)((elapsed_seconds * 100U) / total_seconds);
    }

    screen_format_time(elapsed, sizeof(elapsed), elapsed_seconds);
    if (total_seconds > 0) {
        screen_format_time(total, sizeof(total), total_seconds);
    } else {
        strlcpy(total, "--:--", sizeof(total));
    }
    snprintf(progress_text, sizeof(progress_text), "%s / %s", elapsed, total);

    if (lvgl_port_lock(0)) {
        if (song_progress_bar != NULL) {
            lv_bar_set_value(song_progress_bar, progress, LV_ANIM_OFF);
        }
        if (song_progress_time_label != NULL) {
            lv_label_set_text(song_progress_time_label, progress_text);
        }
        lvgl_port_unlock();
    }
}

static void screen_format_time(char* buf, size_t buf_size, uint32_t seconds) {
    uint32_t minutes = seconds / 60U;
    uint32_t rem_seconds = seconds % 60U;

    snprintf(buf, buf_size, "%lu:%02lu", (unsigned long)minutes, (unsigned long)rem_seconds);
}
