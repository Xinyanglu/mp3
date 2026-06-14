/*
 * Shared Bluetooth app state.
 *
 * Defines the global state variables declared in bt_private.h, including peer
 * identity, A2DP/media state, AVRCP capabilities, counters, and timers.
 */
#include "bt_private.h"

esp_bd_addr_t s_peer_bda = {0};
char s_peer_bdname[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
int s_a2d_state = APP_AV_STATE_IDLE;
int s_media_state = APP_AV_MEDIA_STATE_IDLE;
int s_intv_cnt = 0;
int s_connecting_intv = 0;
uint32_t s_pkt_cnt = 0;
esp_avrc_rn_evt_cap_mask_t s_avrc_peer_rn_cap;
TimerHandle_t s_heartbeat_timer;
