/*
 * Public Bluetooth app entry point.
 *
 * Exposes the single initializer used by app_main() to bring up the Bluetooth
 * controller, Bluedroid host stack, A2DP source, AVRCP, GAP discovery, and the
 * app work queue.
 */
#pragma once

#include <stdint.h>

#include <esp_bt_defs.h>
#include "esp_err.h"

typedef struct {
    uint32_t sample_rate;
    uint8_t bits_per_sample;
    uint8_t channels;
} bt_app_audio_info_t;

void init_bt_app(void);
void bt_app_connect_to(const char* name, esp_bd_addr_t bda);
void bt_app_start_media(void);
esp_err_t bt_app_set_audio_info(const bt_app_audio_info_t* info);
esp_err_t bt_app_volume_up(void);
esp_err_t bt_app_volume_down(void);
