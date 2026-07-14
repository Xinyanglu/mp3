#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SDCARD_SONG_PAGE_SIZE 8
#define SDCARD_MAX_SONGS SDCARD_SONG_PAGE_SIZE
#define SDCARD_MAX_NAME_LEN 128
#define SDCARD_MAX_PATH_LEN 256

typedef struct {
    char name[SDCARD_MAX_NAME_LEN];
} sdcard_song_t;

esp_err_t sdcard_init(void);
esp_err_t sdcard_load_song_page(size_t page_index);
size_t sdcard_get_song_count(void);
size_t sdcard_get_total_song_count(void);
size_t sdcard_get_song_page(void);
size_t sdcard_get_total_song_pages(void);
bool sdcard_has_prev_page(void);
bool sdcard_has_next_page(void);
const sdcard_song_t* sdcard_get_song(size_t index);
esp_err_t sdcard_get_song_path(size_t index, char* path, size_t path_size);
