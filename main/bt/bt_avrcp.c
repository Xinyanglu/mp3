/*
 * AVRCP controller handling.
 *
 * Handles remote-control events from the sink, including connection state,
 * remote notification capabilities, and volume-change notifications.
 */
#include "bt_private.h"

#include <inttypes.h>
#include <stdlib.h>

#include "bt_app.h"
#include "bt_app_core.h"
#include "esp_err.h"
#include "esp_log.h"

#define BT_VOLUME_STEP 8
#define BT_VOLUME_MAX 127

static void bt_av_hdl_avrc_ct_evt(uint16_t event, void* p_param);
static void bt_av_volume_change_hdlr(uint16_t event, void* p_param);
static void bt_av_notify_evt_handler(uint8_t event_id, esp_avrc_rn_param_t* event_parameter);

static uint8_t s_volume = BT_VOLUME_MAX / 2;

void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t* param) {
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
    case ESP_AVRC_CT_METADATA_RSP_EVT:
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
    case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT:
    case ESP_AVRC_CT_PROF_STATE_EVT:
        bt_app_work_dispatch(bt_av_hdl_avrc_ct_evt, event, param, sizeof(esp_avrc_ct_cb_param_t), NULL);
        break;
    default:
        ESP_LOGE(BT_RC_CT_TAG, "Invalid AVRC event: %d", event);
        break;
    }
}

static void bt_av_register_volume_change_notification(void) {
    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &s_avrc_peer_rn_cap, ESP_AVRC_RN_VOLUME_CHANGE)) {
        esp_avrc_ct_send_register_notification_cmd(APP_RC_CT_TL_RN_VOLUME_CHANGE, ESP_AVRC_RN_VOLUME_CHANGE, 0);
    }
}

static void bt_av_notify_evt_handler(uint8_t event_id, esp_avrc_rn_param_t* event_parameter) {
    switch (event_id) {
    case ESP_AVRC_RN_VOLUME_CHANGE:
        ESP_LOGI(BT_RC_CT_TAG, "Volume changed: %d", event_parameter->volume);
        s_volume = event_parameter->volume;
        bt_av_register_volume_change_notification();
        break;
    default:
        break;
    }
}

static void bt_av_volume_change_hdlr(uint16_t event, void* p_param) {
    int8_t delta;
    int next_volume;

    (void)event;

    if (p_param == NULL) {
        return;
    }

    delta = *(int8_t*)p_param;
    next_volume = (int)s_volume + delta;
    if (next_volume < 0) {
        next_volume = 0;
    } else if (next_volume > BT_VOLUME_MAX) {
        next_volume = BT_VOLUME_MAX;
    }

    s_volume = (uint8_t)next_volume;
    ESP_LOGI(BT_RC_CT_TAG, "Set absolute volume: volume %d", s_volume);
    esp_avrc_ct_send_set_absolute_volume_cmd(APP_RC_CT_TL_SET_VOLUME, s_volume);
}

esp_err_t bt_app_volume_up(void) {
    int8_t delta = BT_VOLUME_STEP;
    return bt_app_work_dispatch(bt_av_volume_change_hdlr, 0, &delta, sizeof(delta), NULL) ? ESP_OK : ESP_FAIL;
}

esp_err_t bt_app_volume_down(void) {
    int8_t delta = -BT_VOLUME_STEP;
    return bt_app_work_dispatch(bt_av_volume_change_hdlr, 0, &delta, sizeof(delta), NULL) ? ESP_OK : ESP_FAIL;
}

static void bt_av_hdl_avrc_ct_evt(uint16_t event, void* p_param) {
    ESP_LOGI(BT_RC_CT_TAG, "%s evt %d", __func__, event);
    esp_avrc_ct_cb_param_t* rc = (esp_avrc_ct_cb_param_t*)(p_param);

    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT: {
        uint8_t* bda = rc->conn_stat.remote_bda;
        ESP_LOGI(BT_RC_CT_TAG,
                 "AVRC conn_state event: state %d, [%02x:%02x:%02x:%02x:%02x:%02x]",
                 rc->conn_stat.connected,
                 bda[0],
                 bda[1],
                 bda[2],
                 bda[3],
                 bda[4],
                 bda[5]);

        if (rc->conn_stat.connected) {
            esp_avrc_ct_send_get_rn_capabilities_cmd(APP_RC_CT_TL_GET_CAPS);
        } else {
            s_avrc_peer_rn_cap.bits = 0;
        }
        break;
    }
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
        ESP_LOGI(BT_RC_CT_TAG,
                 "AVRC passthrough response: key_code 0x%x, key_state %d, rsp_code %d",
                 rc->psth_rsp.key_code,
                 rc->psth_rsp.key_state,
                 rc->psth_rsp.rsp_code);
        break;
    case ESP_AVRC_CT_METADATA_RSP_EVT:
        ESP_LOGI(BT_RC_CT_TAG,
                 "AVRC metadata response: attribute id 0x%x, %s",
                 rc->meta_rsp.attr_id,
                 rc->meta_rsp.attr_text);
        free(rc->meta_rsp.attr_text);
        break;
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        ESP_LOGI(BT_RC_CT_TAG, "AVRC event notification: %d", rc->change_ntf.event_id);
        bt_av_notify_evt_handler(rc->change_ntf.event_id, &rc->change_ntf.event_parameter);
        break;
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
        ESP_LOGI(BT_RC_CT_TAG,
                 "AVRC remote features %" PRIx32 ", TG features %x",
                 rc->rmt_feats.feat_mask,
                 rc->rmt_feats.tg_feat_flag);
        break;
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        ESP_LOGI(BT_RC_CT_TAG,
                 "remote rn_cap: count %d, bitmask 0x%x",
                 rc->get_rn_caps_rsp.cap_count,
                 rc->get_rn_caps_rsp.evt_set.bits);
        s_avrc_peer_rn_cap.bits = rc->get_rn_caps_rsp.evt_set.bits;
        bt_av_register_volume_change_notification();
        break;
    case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT:
        ESP_LOGI(BT_RC_CT_TAG, "Set absolute volume response: volume %d", rc->set_volume_rsp.volume);
        s_volume = rc->set_volume_rsp.volume;
        break;
    case ESP_AVRC_CT_PROF_STATE_EVT:
        if (ESP_AVRC_INIT_SUCCESS == rc->avrc_ct_init_stat.state) {
            ESP_LOGI(BT_RC_CT_TAG, "AVRCP CT STATE: Init Complete");
        } else if (ESP_AVRC_DEINIT_SUCCESS == rc->avrc_ct_init_stat.state) {
            ESP_LOGI(BT_RC_CT_TAG, "AVRCP CT STATE: Deinit Complete");
        } else {
            ESP_LOGE(BT_RC_CT_TAG, "AVRCP CT STATE error: %d", rc->avrc_ct_init_stat.state);
        }
        break;
    default:
        ESP_LOGE(BT_RC_CT_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
}
