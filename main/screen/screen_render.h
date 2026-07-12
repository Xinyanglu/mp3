#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_bt_defs.h"
#include "esp_err.h"
#include "screen.h"

typedef struct {
    char name[MAX_DEVICE_NAME_LEN];
    esp_bd_addr_t bda;
    int64_t last_discovered_ms;
} screen_bt_device_t;

esp_err_t screen_render_init(void);
void screen_render_bt_scan(const screen_bt_device_t* bt_devices,
                           int max_bt_devices,
                           uint8_t num_bt_devices,
                           int selected_bt_device_idx);
void screen_render_song_selection(int selected_song_idx);
void screen_render_song_loading(int selected_song_idx);
void screen_render_song_playing(int selected_song_idx);
void screen_render_song_paused(bool paused);
void screen_render_song_progress(uint32_t elapsed_seconds, uint32_t total_seconds);
