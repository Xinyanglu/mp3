/*
 * Bluetooth stack and profile initialization.
 *
 * Owns NVS setup, controller/Bluedroid startup, profile registration, and the
 * initial stack-up event that kicks off device discovery and media setup.
 */
#include "bt_app.h"

#include "bt_app_core.h"
#include "bt_private.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

void bt_av_hdl_stack_evt(uint16_t event, void* p_param __attribute__((unused))) {
    bt_log_enter(__func__);
    ESP_LOGI(BT_AV_TAG, "%s event: %d", __func__, event);

    switch (event) {
    case BT_APP_STACK_UP_EVT: {
        char* dev_name = LOCAL_DEVICE_NAME;
        esp_bt_gap_set_device_name(dev_name);
        esp_bt_gap_register_callback(bt_app_gap_cb);

        esp_avrc_ct_init();
        esp_avrc_ct_register_callback(bt_app_rc_ct_cb);

        esp_avrc_rn_evt_cap_mask_t evt_set = {0};
        esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
        ESP_ERROR_CHECK(esp_avrc_tg_set_rn_evt_cap(&evt_set));

        esp_a2d_source_init();
        esp_a2d_register_callback(bt_app_a2d_cb);
        esp_a2d_source_register_data_callback(bt_app_a2d_data_cb);

        esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        esp_bt_gap_get_device_name();

        ESP_LOGI(BT_AV_TAG, "Starting device discovery...");
        s_a2d_state = APP_AV_STATE_DISCOVERING;
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);

        s_heartbeat_timer = xTimerCreate("connTmr", pdMS_TO_TICKS(10000), pdTRUE, NULL, bt_app_a2d_heart_beat);
        xTimerStart(s_heartbeat_timer, portMAX_DELAY);
        break;
    }
    default:
        ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
    bt_log_leave(__func__);
}

void init_bt_app(void) {
    char bda_str[18] = {0};
    esp_err_t ret;

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&bt_cfg) != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s initialize controller failed", __func__);
        return;
    }
    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s enable controller failed with rc: 0x%.03x", __func__, ret);
        return;
    }

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();

    if ((ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg)) != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s initialize bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    if (esp_bluedroid_enable() != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s enable bluedroid failed", __func__);
        return;
    }

    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));

    ESP_LOGI(BT_AV_TAG, "Own address:[%s]", bt_bda2str((uint8_t*)esp_bt_dev_get_address(), bda_str, sizeof(bda_str)));
    bt_app_task_start_up();
    bt_app_work_dispatch(bt_av_hdl_stack_evt, BT_APP_STACK_UP_EVT, NULL, 0, NULL);
}
