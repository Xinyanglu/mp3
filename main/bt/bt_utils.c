/*
 * Bluetooth utility helpers.
 *
 * Contains shared logging helpers and address formatting used by the GAP, A2DP,
 * and AVRCP modules.
 */
#include "bt_private.h"

#include <stdio.h>

#include "esp_log.h"

void bt_log_enter(const char *func)
{
    ESP_LOGI(BT_AV_TAG, "%s --> entering", func);
}

void bt_log_leave(const char *func)
{
    ESP_LOGI(BT_AV_TAG, "%s --> leaving", func);
}

char *bt_bda2str(esp_bd_addr_t bda, char *str, size_t size)
{
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }

    snprintf(str, size, "%02x:%02x:%02x:%02x:%02x:%02x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return str;
}
