/*
 * Bluetooth GAP discovery logic.
 *
 * Scans for nearby Classic Bluetooth devices, extracts advertised names, and
 * starts the A2DP connection flow when the configured target device is found.
 */
#include "bt_private.h"

#include <inttypes.h>
#include <string.h>

#include "bt_app_core.h"
#include "esp_a2dp_api.h"
#include "esp_bt_defs.h"
#include "esp_log.h"

#include "screen.h"

typedef struct {
    esp_bd_addr_t bda;
    char bdname[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
    uint32_t cod;
    bool has_bdname;
    bool has_cod;
} bt_app_gap_disc_res_t;

typedef struct {
    union {
        bt_app_gap_disc_res_t disc_res;
        esp_bt_gap_discovery_state_t disc_state;
    };
} bt_app_gap_msg_t;

typedef struct {
    char name[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
    esp_bd_addr_t bda;
} bt_app_connect_msg_t;

static void bt_app_connect_to_handler(uint16_t event, void* param);
static const char* gap_event_to_str(uint16_t event);
static void bt_app_gap_build_msg(bt_app_gap_msg_t* msg, esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param);

static const char* const s_gap_event_names[] = {
    [ESP_BT_GAP_DISC_RES_EVT] = "ESP_BT_GAP_DISC_RES_EVT",
    [ESP_BT_GAP_DISC_STATE_CHANGED_EVT] = "ESP_BT_GAP_DISC_STATE_CHANGED_EVT",
    [ESP_BT_GAP_RMT_SRVCS_EVT] = "ESP_BT_GAP_RMT_SRVCS_EVT",
    [ESP_BT_GAP_RMT_SRVC_REC_EVT] = "ESP_BT_GAP_RMT_SRVC_REC_EVT",
    [ESP_BT_GAP_AUTH_CMPL_EVT] = "ESP_BT_GAP_AUTH_CMPL_EVT",
    [ESP_BT_GAP_PIN_REQ_EVT] = "ESP_BT_GAP_PIN_REQ_EVT",
    [ESP_BT_GAP_CFM_REQ_EVT] = "ESP_BT_GAP_CFM_REQ_EVT",
    [ESP_BT_GAP_KEY_NOTIF_EVT] = "ESP_BT_GAP_KEY_NOTIF_EVT",
    [ESP_BT_GAP_KEY_REQ_EVT] = "ESP_BT_GAP_KEY_REQ_EVT",
    [ESP_BT_GAP_READ_RSSI_DELTA_EVT] = "ESP_BT_GAP_READ_RSSI_DELTA_EVT",
    [ESP_BT_GAP_CONFIG_EIR_DATA_EVT] = "ESP_BT_GAP_CONFIG_EIR_DATA_EVT",
    [ESP_BT_GAP_SET_AFH_CHANNELS_EVT] = "ESP_BT_GAP_SET_AFH_CHANNELS_EVT",
    [ESP_BT_GAP_READ_REMOTE_NAME_EVT] = "ESP_BT_GAP_READ_REMOTE_NAME_EVT",
    [ESP_BT_GAP_MODE_CHG_EVT] = "ESP_BT_GAP_MODE_CHG_EVT",
    [ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT] = "ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT",
    [ESP_BT_GAP_QOS_CMPL_EVT] = "ESP_BT_GAP_QOS_CMPL_EVT",
    [ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT] = "ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT",
    [ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT] = "ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT",
    [ESP_BT_GAP_SET_PAGE_TO_EVT] = "ESP_BT_GAP_SET_PAGE_TO_EVT",
    [ESP_BT_GAP_GET_PAGE_TO_EVT] = "ESP_BT_GAP_GET_PAGE_TO_EVT",
    [ESP_BT_GAP_ACL_PKT_TYPE_CHANGED_EVT] = "ESP_BT_GAP_ACL_PKT_TYPE_CHANGED_EVT",
    [ESP_BT_GAP_ENC_CHG_EVT] = "ESP_BT_GAP_ENC_CHG_EVT",
    [ESP_BT_GAP_SET_MIN_ENC_KEY_SIZE_EVT] = "ESP_BT_GAP_SET_MIN_ENC_KEY_SIZE_EVT",
    [ESP_BT_GAP_GET_DEV_NAME_CMPL_EVT] = "ESP_BT_GAP_GET_DEV_NAME_CMPL_EVT",
};

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

static void filter_inquiry_scan_result(bt_app_gap_disc_res_t* disc_res) {
    char bda_str[18];

    ESP_LOGI(BT_AV_TAG, "Scanned device: %s", bt_bda2str((uint8_t*)disc_res->bda, bda_str, sizeof(bda_str)));

    if (disc_res->has_cod) {
        ESP_LOGI(BT_AV_TAG, "--Class of Device: 0x%" PRIx32, disc_res->cod);
    }

    if (!disc_res->has_cod || !esp_bt_gap_is_valid_cod(disc_res->cod) ||
        !(esp_bt_gap_get_cod_srvc(disc_res->cod) & ESP_BT_COD_SRVC_RENDERING)) {
        return;
    }

    if (disc_res->has_bdname) {
        strncpy(s_peer_bdname, disc_res->bdname, sizeof(s_peer_bdname) - 1);
        s_peer_bdname[sizeof(s_peer_bdname) - 1] = '\0';
        ESP_LOGI(BT_AV_TAG, "Found device name: %s", s_peer_bdname);
        screen_notify_bt_device_found(s_peer_bdname, disc_res->bda);
        screen_notify_bt_refresh(NULL);
    }
}

void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    bt_app_gap_msg_t msg;

    bt_app_gap_build_msg(&msg, event, param);
    bt_app_work_dispatch(bt_app_gap_sm_hdlr, event, &msg, sizeof(msg), NULL);
}

void bt_app_connect_to(const char* name, esp_bd_addr_t bda) {
    if (!name || !bda) {
        return;
    }

    bt_app_connect_msg_t msg;
    memcpy(msg.bda, bda, sizeof(esp_bd_addr_t));
    strncpy(msg.name, name, sizeof(msg.name) - 1);
    msg.name[sizeof(msg.name) - 1] = '\0';
    bt_app_work_dispatch(bt_app_connect_to_handler, 0, &msg, sizeof(msg), NULL);
}

void bt_app_gap_sm_hdlr(uint16_t event, void* param) {
    bt_app_gap_msg_t* msg = (bt_app_gap_msg_t*)param;

    bt_log_enter(__func__);
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT:
        if (s_a2d_state == APP_AV_STATE_DISCOVERING) {
            filter_inquiry_scan_result(&msg->disc_res);
        }
        break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (msg->disc_state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            if (s_a2d_state == APP_AV_STATE_DISCOVERED) {
                s_a2d_state = APP_AV_STATE_CONNECTING;
                ESP_LOGI(BT_AV_TAG, "Device discovery stopped.");
                ESP_LOGI(BT_AV_TAG, "a2dp connecting to peer: %s", s_peer_bdname);
                esp_a2d_source_connect(s_peer_bda);
            } else if (s_a2d_state == APP_AV_STATE_DISCOVERING) {
                ESP_LOGI(BT_AV_TAG, "Device discovery failed, continue to discover...");
                esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
            }
        } else if (msg->disc_state == ESP_BT_GAP_DISCOVERY_STARTED) {
            ESP_LOGI(BT_AV_TAG, "Discovery started.");
        }
        break;
    default:
        ESP_LOGI(BT_AV_TAG, "event: %s (0x%x)", gap_event_to_str(event), event);
        break;
    }
    bt_log_leave(__func__);
}

static void bt_app_connect_to_handler(uint16_t event __attribute__((unused)), void* param) {
    if (s_a2d_state != APP_AV_STATE_DISCOVERING)
        return;

    bt_app_connect_msg_t* msg = (bt_app_connect_msg_t*)param;
    char bda_str[18] = {0};

    ESP_LOGI(BT_AV_TAG, "Connecting to %s [%s]", msg->name, bt_bda2str(msg->bda, bda_str, sizeof(bda_str)));
    memcpy(s_peer_bda, msg->bda, sizeof(esp_bd_addr_t));
    strncpy(s_peer_bdname, msg->name, sizeof(s_peer_bdname) - 1);
    s_peer_bdname[sizeof(s_peer_bdname) - 1] = '\0';

    s_a2d_state = APP_AV_STATE_DISCOVERED;
    esp_bt_gap_cancel_discovery();
}

static const char* gap_event_to_str(uint16_t event) {
    if (event >= (sizeof(s_gap_event_names) / sizeof(s_gap_event_names[0])) || s_gap_event_names[event] == NULL) {
        return "UNKNOWN";
    }

    return s_gap_event_names[event];
}

static void bt_app_gap_copy_bdname(char* dst, size_t dst_size, const void* src, int src_len) {
    int copy_len = src_len;

    if (dst == NULL || dst_size == 0 || src == NULL || src_len <= 0) {
        return;
    }

    if ((size_t)copy_len >= dst_size) {
        copy_len = dst_size - 1;
    }

    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
}

static void bt_app_gap_build_disc_res(bt_app_gap_disc_res_t* disc_res, const esp_bt_gap_cb_param_t* param) {
    esp_bt_gap_dev_prop_t* p;

    memcpy(disc_res->bda, param->disc_res.bda, sizeof(disc_res->bda));

    if (param->disc_res.prop == NULL || param->disc_res.num_prop <= 0) {
        return;
    }

    for (int i = 0; i < param->disc_res.num_prop; i++) {
        p = param->disc_res.prop + i;
        switch (p->type) {
        case ESP_BT_GAP_DEV_PROP_COD:
            if (p->val != NULL && p->len >= (int)sizeof(disc_res->cod)) {
                disc_res->cod = *(uint32_t*)p->val;
                disc_res->has_cod = true;
            }
            break;
        case ESP_BT_GAP_DEV_PROP_EIR:
            if (get_name_from_eir((uint8_t*)p->val, disc_res->bdname, NULL)) {
                disc_res->has_bdname = true;
            }
            break;
        case ESP_BT_GAP_DEV_PROP_BDNAME:
            if (!disc_res->has_bdname && p->val != NULL && p->len > 0) {
                bt_app_gap_copy_bdname(disc_res->bdname, sizeof(disc_res->bdname), p->val, p->len);
                disc_res->has_bdname = true;
            }
            break;
        default:
            break;
        }
    }
}

static void bt_app_gap_build_msg(bt_app_gap_msg_t* msg, esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    memset(msg, 0, sizeof(*msg));

    if (param == NULL) {
        return;
    }

    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT:
        bt_app_gap_build_disc_res(&msg->disc_res, param);
        break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        msg->disc_state = param->disc_st_chg.state;
        break;
    default:
        break;
    }
}
