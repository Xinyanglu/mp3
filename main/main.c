#include "bt_app.h"
#include "buttons.h"
#include "esp_err.h"
#include "esp_log.h"
#include "screen.h"
#include "sdcard.h"

static const char* TAG = "main";

void app_main(void) {
    ESP_ERROR_CHECK(screen_init());
    ESP_ERROR_CHECK(buttons_init());
    ESP_ERROR_CHECK(sdcard_init());
    ESP_ERROR_CHECK(sdcard_scan_songs());

    screen_notify_bt_refresh(NULL);
    init_bt_app();

    ESP_LOGI(TAG, "LVGL hello world ready");
}
