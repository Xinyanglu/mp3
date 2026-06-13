/*
 * Internal Bluetooth module interface.
 *
 * Shares constants, state declarations, and private cross-module function
 * prototypes between the GAP, A2DP, AVRCP, app setup, and utility files.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_gap_bt_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#define BT_AV_TAG "BT_AV"
#define BT_RC_CT_TAG "RC_CT"

#define LOCAL_DEVICE_NAME "ESP_A2DP_SRC"

#define APP_RC_CT_TL_GET_CAPS 0
#define APP_RC_CT_TL_RN_VOLUME_CHANGE 1

enum {
    BT_APP_STACK_UP_EVT = 0x0000,
    BT_APP_HEART_BEAT_EVT = 0xff00,
};

enum {
    APP_AV_STATE_IDLE,
    APP_AV_STATE_DISCOVERING,
    APP_AV_STATE_DISCOVERED,
    APP_AV_STATE_UNCONNECTED,
    APP_AV_STATE_CONNECTING,
    APP_AV_STATE_CONNECTED,
    APP_AV_STATE_DISCONNECTING,
};

enum {
    APP_AV_MEDIA_STATE_IDLE,
    APP_AV_MEDIA_STATE_STARTING,
    APP_AV_MEDIA_STATE_STARTED,
    APP_AV_MEDIA_STATE_STOPPING,
};

extern esp_bd_addr_t s_peer_bda;
extern char s_peer_bdname[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
extern int s_a2d_state;
extern int s_media_state;
extern int s_intv_cnt;
extern int s_connecting_intv;
extern uint32_t s_pkt_cnt;
extern esp_avrc_rn_evt_cap_mask_t s_avrc_peer_rn_cap;
extern TimerHandle_t s_heartbeat_timer;
extern const char remote_device_name[];

char *bt_bda2str(esp_bd_addr_t bda, char *str, size_t size);
void bt_log_enter(const char *func);
void bt_log_leave(const char *func);

void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
int32_t bt_app_a2d_data_cb(uint8_t *data, int32_t len);
void bt_app_a2d_heart_beat(TimerHandle_t arg);
void bt_app_av_sm_hdlr(uint16_t event, void *param);
void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);
void bt_av_hdl_stack_evt(uint16_t event, void *p_param);
