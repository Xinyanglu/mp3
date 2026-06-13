#include "bt_app.h"
#include "esp_err.h"
#include "esp_log.h"
#include "screen.h"

static const char* TAG = "main";

void app_main(void) {
    ESP_ERROR_CHECK(screen_init());
    screen_show_bt_scan();
    init_bt_app();

    ESP_LOGI(TAG, "LVGL hello world ready");
}
