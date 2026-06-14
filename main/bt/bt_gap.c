/*
 * Bluetooth GAP discovery logic.
 *
 * Scans for nearby Classic Bluetooth devices, extracts advertised names, and
 * starts the A2DP connection flow when the configured target device is found.
 */
#include "bt_private.h"

#include <inttypes.h>
#include <string.h>

#include "esp_a2dp_api.h"
#include "esp_log.h"

#include "screen.h"

static bool get_name_from_eir(uint8_t* eir, char* bdname, uint8_t* bdname_len) {
    uint8_t* rmt_bdname = NULL;
    uint8_t rmt_bdname_len = 0;

    if (!eir) {
        return false;
    }

    rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
    if (!rmt_bdname) {
        rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);
    }

    if (rmt_bdname) {
        if (rmt_bdname_len > ESP_BT_GAP_MAX_BDNAME_LEN) {
            rmt_bdname_len = ESP_BT_GAP_MAX_BDNAME_LEN;
        }

        if (bdname) {
            memcpy(bdname, rmt_bdname, rmt_bdname_len);
            bdname[rmt_bdname_len] = '\0';
        }
        if (bdname_len) {
            *bdname_len = rmt_bdname_len;
        }
        return true;
    }

    return false;
}

static void filter_inquiry_scan_result(esp_bt_gap_cb_param_t* param) {
    char bda_str[18];
    uint32_t cod = 0;
    int32_t rssi = -129;
    uint8_t* eir = NULL;
    esp_bt_gap_dev_prop_t* p;

    ESP_LOGI(BT_AV_TAG, "Scanned device: %s", bt_bda2str(param->disc_res.bda, bda_str, sizeof(bda_str)));
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        p = param->disc_res.prop + i;
        switch (p->type) {
        case ESP_BT_GAP_DEV_PROP_COD:
            cod = *(uint32_t*)(p->val);
            ESP_LOGI(BT_AV_TAG, "--Class of Device: 0x%" PRIx32, cod);
            break;
        case ESP_BT_GAP_DEV_PROP_RSSI:
            rssi = *(int8_t*)(p->val);
            ESP_LOGI(BT_AV_TAG, "--RSSI: %" PRId32, rssi);
            break;
        case ESP_BT_GAP_DEV_PROP_EIR:
            eir = (uint8_t*)(p->val);
            break;
        case ESP_BT_GAP_DEV_PROP_BDNAME:
        default:
            break;
        }
    }

    if (!esp_bt_gap_is_valid_cod(cod) || !(esp_bt_gap_get_cod_srvc(cod) & ESP_BT_COD_SRVC_RENDERING)) {
        return;
    }

    if (eir) {
        get_name_from_eir(eir, s_peer_bdname, NULL);
        ESP_LOGI(BT_AV_TAG, "Found device name: %s", s_peer_bdname);
        screen_notify_bt_device_found(s_peer_bdname, param->disc_res.bda);
        screen_notify_bt_refresh(NULL);
    }
}

void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    bt_log_enter(__func__);
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT:
        if (s_a2d_state == APP_AV_STATE_DISCOVERING) {
            filter_inquiry_scan_result(param);
        }
        break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            if (s_a2d_state == APP_AV_STATE_DISCOVERED) {
                s_a2d_state = APP_AV_STATE_CONNECTING;
                ESP_LOGI(BT_AV_TAG, "Device discovery stopped.");
                ESP_LOGI(BT_AV_TAG, "a2dp connecting to peer: %s", s_peer_bdname);
                esp_a2d_source_connect(s_peer_bda);
            } else {
                ESP_LOGI(BT_AV_TAG, "Device discovery failed, continue to discover...");
                esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
            }
        } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            ESP_LOGI(BT_AV_TAG, "Discovery started.");
        }
        break;
    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_MODE_CHG_EVT mode: %d", param->mode_chg.mode);
        break;
    case ESP_BT_GAP_GET_DEV_NAME_CMPL_EVT:
        if (param->get_dev_name_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_GET_DEV_NAME_CMPL_EVT device name: %s", param->get_dev_name_cmpl.name);
        } else {
            ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_GET_DEV_NAME_CMPL_EVT failed, state: %d", param->get_dev_name_cmpl.status);
        }
        break;
    default:
        ESP_LOGI(BT_AV_TAG, "event: %d", event);
        break;
    }
    bt_log_leave(__func__);
}
