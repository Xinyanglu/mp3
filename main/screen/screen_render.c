/*
 * LCD and LVGL rendering.
 *
 * Owns display setup and drawing for the screen state machine.
 */
#include "screen_render.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "sdcard.h"

#define LCD_HOST SPI2_HOST

#define LCD_H_RES 320
#define LCD_V_RES 240
#define LCD_DRAW_BUFF_HEIGHT 20
#define SONG_SELECT_ROW_HEIGHT 24
#define SONG_SELECT_LABEL_HEIGHT 20

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

static const char* TAG = "screen_render";

static esp_lcd_panel_io_handle_t lcd_io;
static esp_lcd_panel_handle_t lcd_panel;
static lv_display_t* display;
static lv_obj_t* song_title_label;
static lv_obj_t* song_progress_bar;
static lv_obj_t* song_progress_time_label;

static esp_err_t init_lcd(void);
static esp_err_t init_lvgl(void);
static void screen_format_time(char* buf, size_t buf_size, uint32_t seconds);

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

esp_err_t screen_render_init(void) {
    ESP_RETURN_ON_ERROR(init_lcd(), TAG, "LCD init failed");
    ESP_RETURN_ON_ERROR(init_lvgl(), TAG, "LVGL setup failed");

    return ESP_OK;
}

void screen_render_bt_scan(const screen_bt_device_t* bt_devices,
                           int max_bt_devices,
                           uint8_t num_bt_devices,
                           int selected_bt_device_idx) {
    if (lvgl_port_lock(0)) {
        lv_obj_t* screen = lv_screen_active();
        lv_obj_clean(screen);
        song_title_label = NULL;
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
        for (int i = 0; i < max_bt_devices; i++) {
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

void screen_render_song_selection(size_t selected_song_idx) {
    size_t songs_count = sdcard_get_song_count();
    size_t total_songs = sdcard_get_total_song_count();
    size_t total_pages = sdcard_get_total_song_pages();

    if (lvgl_port_lock(0)) {
        lv_obj_t* screen = lv_screen_active();
        lv_obj_clean(screen);
        song_title_label = NULL;
        song_progress_bar = NULL;
        song_progress_time_label = NULL;
        lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t* title = lv_label_create(screen);
        if (total_songs > 0) {
            lv_label_set_text_fmt(title,
                                  "Songs: %u  Page %u/%u",
                                  (unsigned int)total_songs,
                                  (unsigned int)(sdcard_get_song_page() + 1),
                                  (unsigned int)total_pages);
        } else {
            lv_label_set_text(title, "Songs: 0");
        }
        lv_obj_set_style_text_color(title, lv_color_hex(0x202020), LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 12);

        int y = 40;
        for (size_t i = 0; i < songs_count; i++) {
            const sdcard_song_t* song = sdcard_get_song(i);
            if (song == NULL) {
                continue;
            }

            lv_obj_t* song_label = lv_label_create(screen);
            lv_obj_set_size(song_label, LCD_H_RES - 24, SONG_SELECT_LABEL_HEIGHT);
            lv_label_set_long_mode(song_label, LV_LABEL_LONG_DOT);
            lv_label_set_text(song_label, song->name);

            if (i == selected_song_idx) {
                lv_obj_set_style_bg_color(song_label, lv_color_hex(0x202020), LV_PART_MAIN);
                lv_obj_set_style_bg_opa(song_label, LV_OPA_COVER, LV_PART_MAIN);
                lv_obj_set_style_text_color(song_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            } else {
                lv_obj_set_style_text_color(song_label, lv_color_hex(0x202020), LV_PART_MAIN);
            }

            lv_obj_align(song_label, LV_ALIGN_TOP_LEFT, 12, y);
            y += SONG_SELECT_ROW_HEIGHT;
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

void screen_render_song_loading(size_t selected_song_idx) {
    const sdcard_song_t* song = sdcard_get_song(selected_song_idx);

    if (lvgl_port_lock(0)) {
        lv_obj_t* screen = lv_screen_active();
        lv_obj_clean(screen);
        song_title_label = NULL;
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

void screen_render_song_playing(size_t selected_song_idx) {
    const sdcard_song_t* song = sdcard_get_song(selected_song_idx);

    if (lvgl_port_lock(0)) {
        lv_obj_t* screen = lv_screen_active();
        lv_obj_clean(screen);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

        song_title_label = lv_label_create(screen);
        lv_label_set_text(song_title_label, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(song_title_label, lv_color_hex(0x202020), LV_PART_MAIN);
        lv_obj_align(song_title_label, LV_ALIGN_TOP_LEFT, 12, 12);

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

void screen_render_song_paused(bool paused) {
    if (lvgl_port_lock(0)) {
        if (song_title_label != NULL) {
            lv_label_set_text(song_title_label, paused ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        }
        lvgl_port_unlock();
    }
}

void screen_render_song_progress(uint32_t elapsed_seconds, uint32_t total_seconds) {
    char elapsed[12];
    char total[12];
    char progress_text[32];
    int progress = 0;

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
    uint32_t minutes     = seconds / 60U;
    uint32_t rem_seconds = seconds % 60U;

    snprintf(buf, buf_size, "%lu:%02lu", (unsigned long)minutes, (unsigned long)rem_seconds);
}
