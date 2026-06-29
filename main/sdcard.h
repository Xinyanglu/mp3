#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

#define SDCARD_MAX_SONGS 64
#define SDCARD_MAX_NAME_LEN 128
#define SDCARD_MAX_PATH_LEN 256

typedef struct {
    char name[SDCARD_MAX_NAME_LEN];
    char path[SDCARD_MAX_PATH_LEN];
    uint32_t size_bytes;
} sdcard_song_t;

typedef struct {
    const char* mount_path;
    spi_host_device_t host;
    gpio_num_t gpio_miso;
    gpio_num_t gpio_mosi;
    gpio_num_t gpio_sclk;
    gpio_num_t gpio_cs;
    int max_files;
    bool format_if_mount_failed;
    int max_freq_khz;
    int max_transfer_sz;
} sdcard_config_t;

esp_err_t sdcard_init(void);
esp_err_t sdcard_mount(void);
esp_err_t sdcard_unmount(void);
esp_err_t sdcard_scan_songs(void);
size_t sdcard_get_song_count(void);
const sdcard_song_t* sdcard_get_song(size_t index);
esp_err_t sdcard_select_song(size_t index);
const sdcard_song_t* sdcard_get_selected_song(void);
