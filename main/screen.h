#pragma once
#include "esp_err.h"

esp_err_t screen_init(void);
void screen_show_hello_world(void);
void screen_show_bt_scan(void);
esp_err_t screen_add_bt_device(const char* device_name);
