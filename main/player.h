#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdcard.h"

typedef struct {
    uint32_t sample_rate;
    uint8_t bits_per_sample;
    uint8_t channels;
    uint32_t bitrate;
} player_audio_info_t;

esp_err_t player_init(void);
esp_err_t player_play(size_t song_idx);
esp_err_t player_pause(void);
esp_err_t player_resume(void);
esp_err_t player_clear(void);
void player_notify_a2dp_started(void);
int32_t player_read_pcm(uint8_t* data, int32_t len);
