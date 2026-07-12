#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SDCARD_MAX_SONGS 64
#define SDCARD_MAX_NAME_LEN 128
#define SDCARD_MAX_PATH_LEN 256

typedef struct {
    char name[SDCARD_MAX_NAME_LEN];
    uint32_t size_bytes;
} sdcard_song_t;

esp_err_t sdcard_init(void);
esp_err_t sdcard_scan_songs(void);
size_t sdcard_get_song_count(void);
const sdcard_song_t* sdcard_get_song(size_t index);
esp_err_t sdcard_get_song_path(size_t index, char* path, size_t path_size);
