#include "screen.h"

#include <stdbool.h>
#include <stdint.h>

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

typedef enum { SCREEN_STATE_BT_DISCOVERY } screen_state_e;

static const char* TAG = "screen";

static esp_lcd_panel_io_handle_t lcd_io;
static esp_lcd_panel_handle_t lcd_panel;
static lv_display_t* display;
static screen_bt_device_t bt_devices[MAX_BT_DEVICE_NUM];
static uint8_t num_bt_devices;
static esp_timer_handle_t bt_scan_refresh_timer;
static screen_state_e current_screen = SCREEN_STATE_BT_DISCOVERY;

static esp_err_t init_lcd(void);
static esp_err_t init_lvgl(void);
static void add_bt_device(const char* name, esp_bd_addr_t bda);
static int find_bt_device(esp_bd_addr_t bda);
static void screen_refresh_bt_scan(void* arg);
static void screen_show_bt_scan(void);
static esp_err_t screen_add_bt_device(const char* name, esp_bd_addr_t bda);

static QueueHandle_t screen_event_queue = NULL;
static TaskHandle_t screen_task_handle = NULL;

static esp_err_t init_lcd(void) {
    const gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_NUM_BK_LIGHT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bk_gpio_config), TAG, "Backlight GPIO config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_NUM_BK_LIGHT, LCD_BK_LIGHT_OFF_LEVEL), TAG, "Backlight off failed");

    const spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_PCLK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_BUFF_HEIGHT * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "SPI bus init failed");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_DC,
        .cs_gpio_num = PIN_NUM_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &lcd_io), TAG,
                        "Panel IO init failed");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
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
        .io_handle = lcd_io,
        .panel_handle = lcd_panel,
        .buffer_size = LCD_H_RES * LCD_DRAW_BUFF_HEIGHT,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
#if LVGL_VERSION_MAJOR >= 9
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif
        .rotation =
            {
                .swap_xy = true,
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
            case SCREEN_EVT_BT_DEVICE_FOUND:
                screen_add_bt_device(msg.device_name, msg.bda);
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
        .name = "bt_scan_refresh",
    };

    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &bt_scan_refresh_timer), TAG,
                        "BT scan refresh timer create failed");

    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(bt_scan_refresh_timer, 5 * 1000 * 1000), TAG,
                        "BT scan refresh timer start failed");

    return ESP_OK;
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

static void screen_remove_old_bt_devices(void) {
    int64_t now = esp_timer_get_time() / 1000;
    for (int i = 0; i < MAX_BT_DEVICE_NUM; i++) {
        if (bt_devices[i].name[0] == '\0')
            continue;
        if (bt_devices[i].last_discovered_ms < (now - MAX_BT_DEVICE_TIMEOUT_MS)) {
            memset(&bt_devices[i], 0, sizeof(screen_bt_device_t));
            num_bt_devices--;
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

static void screen_show_bt_scan(void) {
    if (current_screen != SCREEN_STATE_BT_DISCOVERY) {
        return;
    }
    if (lvgl_port_lock(0)) {
        lv_obj_t* screen = lv_screen_active();
        lv_obj_clean(screen);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t* title = lv_label_create(screen);
        lv_label_set_text_fmt(title, "Discovered devices: %u", num_bt_devices);
        lv_obj_set_style_text_color(title, lv_color_hex(0x202020), LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 12);

        int y = 40;
        bool found_device = false;
        for (int i = 0; i < MAX_BT_DEVICE_NUM; i++) {
            if (bt_devices[i].name[0] == '\0') {
                continue;
            }

            found_device = true;
            lv_obj_t* device_label = lv_label_create(screen);
            lv_obj_set_width(device_label, LCD_H_RES - 24);
            lv_label_set_long_mode(device_label, LV_LABEL_LONG_WRAP);
            lv_label_set_text_fmt(device_label, "%s\n%02X:%02X:%02X:%02X:%02X:%02X", bt_devices[i].name,
                                  bt_devices[i].bda[0], bt_devices[i].bda[1], bt_devices[i].bda[2], bt_devices[i].bda[3],
                                  bt_devices[i].bda[4], bt_devices[i].bda[5]);
            lv_obj_set_style_text_color(device_label, lv_color_hex(0x202020), LV_PART_MAIN);
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
