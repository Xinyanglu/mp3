#pragma once
#include <stdint.h>

#include "esp_bt_defs.h"
#include "esp_err.h"

#define MAX_DEVICE_NAME_LEN 64

typedef enum {
    SCREEN_EVT_REFRESH_BT_SCAN,
    SCREEN_EVT_BT_DEVICE_FOUND,
    SCREEN_EVT_BTN_PRESS,
    SCREEN_EVT_BT_DEVICE_CONNECTED,
    SCREEN_EVT_SONG_PLAYING,
    SCREEN_EVT_SONG_PROGRESS,
} screen_event;

typedef enum {
    SCREEN_STATE_BT_DISCOVERY,
    SCREEN_STATE_SONG_SELECT,
    SCREEN_STATE_SONG_LOADING,
    SCREEN_STATE_SONG_PLAYING
} screen_state;

typedef enum {
    SCREEN_BTN_UP,
    SCREEN_BTN_DOWN,
    SCREEN_BTN_LEFT,
    SCREEN_BTN_RIGHT,
    SCREEN_BTN_SELECT,
    SCREEN_BTN_MAX
} screen_button;

typedef struct {
    screen_event event;
    screen_button button;
    char device_name[MAX_DEVICE_NAME_LEN];
    esp_bd_addr_t bda;
    screen_state screen_state;
    uint32_t elapsed_seconds;
    uint32_t total_seconds;
} screen_msg;

esp_err_t screen_init(void);
void screen_notify_bt_device_found(const char* device_name, esp_bd_addr_t bda);
void screen_notify_bt_refresh(void* arg);
void screen_notify_button_press(screen_button button);
void screen_notify_show_song_selection(void);
void screen_notify_show_song_playing(void);
void screen_notify_song_progress(uint32_t elapsed_seconds, uint32_t total_seconds);
