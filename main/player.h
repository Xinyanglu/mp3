#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdcard.h"

typedef enum {
    PLAYER_STATE_STOPPED,
    PLAYER_STATE_PLAYING,
    PLAYER_STATE_PAUSED,
    PLAYER_STATE_ERROR,
} player_state_t;

typedef struct {
    player_state_t state;
    char path[SDCARD_MAX_PATH_LEN];
    esp_err_t last_error;
} player_status_t;

typedef struct {
    uint32_t sample_rate;
    uint8_t bits_per_sample;
    uint8_t channels;
    uint32_t bitrate;
} player_audio_info_t;

esp_err_t player_init(void);
esp_err_t player_play(size_t song_idx);
esp_err_t player_pause(void);
esp_err_t player_stop(void);
esp_err_t player_get_status(player_status_t* status);
int32_t player_read_pcm(uint8_t* data, int32_t len);
