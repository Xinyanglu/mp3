#pragma once
#include "esp_bt_defs.h"
#include "esp_err.h"

#define MAX_DEVICE_NAME_LEN 64

typedef enum {
    SCREEN_EVT_REFRESH_BT_SCAN,
    SCREEN_EVT_BT_DEVICE_FOUND,
} screen_event;

typedef struct {
    screen_event event;
    char device_name[MAX_DEVICE_NAME_LEN];
    esp_bd_addr_t bda;
} screen_msg;

esp_err_t screen_init(void);
void screen_notify_bt_device_found(const char* device_name, esp_bd_addr_t bda);
void screen_notify_bt_refresh(void* arg);
