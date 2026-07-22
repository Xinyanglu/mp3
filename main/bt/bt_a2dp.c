/*
 * A2DP source profile handling.
 *
 * Handles A2DP connection/media state, preferred SBC codec selection, heartbeat
 * processing, and the audio data callback that supplies samples to the sink.
 */
#include "bt_private.h"

#include <string.h>

#include "bt_app.h"
#include "bt_app_core.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "player.h"
#include "screen.h"

#define BT_APP_AUDIO_INFO_RETRY_INTERVAL_MS 100
#define BT_APP_AUDIO_INFO_RETRY_MAX_ATTEMPTS 5

static bool check_pref_mcc_against_sink_caps(const esp_a2d_mcc_t* sink_caps, const esp_a2d_mcc_t* pref_mcc);
static bool bt_app_a2d_sbc_freq_from_sample_rate(uint32_t sample_rate, uint8_t* freq);
static bool bt_app_a2d_sbc_ch_mode_from_channels(const esp_a2d_cie_sbc_t* caps, uint8_t channels, uint8_t* ch_mode);
static void bt_app_a2d_store_sink_caps(esp_a2d_conn_hdl_t conn_hdl, const esp_a2d_mcc_t* sink_caps);
static void bt_app_a2d_store_audio_info(const bt_app_audio_info_t* info);
static void bt_app_a2d_set_pref_mcc(esp_a2d_conn_hdl_t conn_hdl, const esp_a2d_mcc_t* sink_caps,
                                    const bt_app_audio_info_t* audio_info);
static void bt_app_a2d_handle_disconnected(void);
static void bt_app_av_state_unconnected_hdlr(uint16_t event, void* param);
static void bt_app_av_state_connecting_hdlr(uint16_t event, void* param);
static void bt_app_av_state_connected_hdlr(uint16_t event, void* param);
static void bt_app_av_state_disconnecting_hdlr(uint16_t event, void* param);
static void bt_app_av_media_proc(uint16_t event, void* param);
static const char* bt_app_av_event_to_str(uint16_t event);
static const char* bt_app_av_state_to_str(int state);

static const char* const s_a2d_event_names[] = {
    [ESP_A2D_CONNECTION_STATE_EVT]       = "ESP_A2D_CONNECTION_STATE_EVT",
    [ESP_A2D_AUDIO_STATE_EVT]            = "ESP_A2D_AUDIO_STATE_EVT",
    [ESP_A2D_AUDIO_CFG_EVT]              = "ESP_A2D_AUDIO_CFG_EVT",
    [ESP_A2D_MEDIA_CTRL_ACK_EVT]         = "ESP_A2D_MEDIA_CTRL_ACK_EVT",
    [ESP_A2D_PROF_STATE_EVT]             = "ESP_A2D_PROF_STATE_EVT",
    [ESP_A2D_SEP_REG_STATE_EVT]          = "ESP_A2D_SEP_REG_STATE_EVT",
    [ESP_A2D_SNK_PSC_CFG_EVT]            = "ESP_A2D_SNK_PSC_CFG_EVT",
    [ESP_A2D_SNK_SET_DELAY_VALUE_EVT]    = "ESP_A2D_SNK_SET_DELAY_VALUE_EVT",
    [ESP_A2D_SNK_GET_DELAY_VALUE_EVT]    = "ESP_A2D_SNK_GET_DELAY_VALUE_EVT",
    [ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT] = "ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT",
    [ESP_A2D_REPORT_SNK_CODEC_CAPS_EVT]  = "ESP_A2D_REPORT_SNK_CODEC_CAPS_EVT",
    [ESP_A2D_SRC_SET_PREF_MCC_EVT]       = "ESP_A2D_SRC_SET_PREF_MCC_EVT",
};

static const char* const s_av_state_names[] = {
    [APP_AV_STATE_IDLE]          = "APP_AV_STATE_IDLE",
    [APP_AV_STATE_DISCOVERING]   = "APP_AV_STATE_DISCOVERING",
    [APP_AV_STATE_DISCOVERED]    = "APP_AV_STATE_DISCOVERED",
    [APP_AV_STATE_UNCONNECTED]   = "APP_AV_STATE_UNCONNECTED",
    [APP_AV_STATE_CONNECTING]    = "APP_AV_STATE_CONNECTING",
    [APP_AV_STATE_CONNECTED]     = "APP_AV_STATE_CONNECTED",
    [APP_AV_STATE_DISCONNECTING] = "APP_AV_STATE_DISCONNECTING",
};

static esp_a2d_mcc_t s_sink_caps;
static esp_a2d_conn_hdl_t s_sink_conn_hdl;
static volatile bool s_sink_caps_valid;
static bt_app_audio_info_t s_audio_info;

void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param) {
    bt_app_work_dispatch(bt_app_av_sm_hdlr, event, param, sizeof(esp_a2d_cb_param_t), NULL);
}

int32_t bt_app_a2d_data_cb(uint8_t* data, int32_t len) {
    if (data == NULL || len < 0) {
        return 0;
    }

    return player_read_pcm(data, len);
}

void bt_app_a2d_heart_beat(TimerHandle_t arg) {
    bt_app_work_dispatch(bt_app_av_sm_hdlr, BT_APP_HEART_BEAT_EVT, NULL, 0, NULL);
}

void bt_app_start_media(void) {
    bt_app_work_dispatch(bt_app_av_sm_hdlr, BT_APP_MEDIA_START_EVT, NULL, 0, NULL);
}

esp_err_t bt_app_set_audio_info(const bt_app_audio_info_t* info) {
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint8_t attempt = 0; !s_sink_caps_valid && attempt < BT_APP_AUDIO_INFO_RETRY_MAX_ATTEMPTS; attempt++) {
        ESP_LOGW(BT_AV_TAG,
                 "Sink capabilities unavailable, waiting before audio info dispatch: %u/%u",
                 attempt + 1,
                 BT_APP_AUDIO_INFO_RETRY_MAX_ATTEMPTS);
        vTaskDelay(pdMS_TO_TICKS(BT_APP_AUDIO_INFO_RETRY_INTERVAL_MS));
    }

    if (!s_sink_caps_valid) {
        ESP_LOGW(BT_AV_TAG, "Timed out waiting for sink capabilities before audio info dispatch");
        return ESP_ERR_TIMEOUT;
    }

    return bt_app_work_dispatch(bt_app_av_sm_hdlr, BT_APP_AUDIO_INFO_EVT, (void*)info, sizeof(*info), NULL) ? ESP_OK
                                                                                                            : ESP_FAIL;
}

void bt_app_av_sm_hdlr(uint16_t event, void* param) {
    ESP_LOGI(BT_AV_TAG,
             "%s state: %s (%d), event: %s (0x%x)",
             __func__,
             bt_app_av_state_to_str(s_a2d_state),
             s_a2d_state,
             bt_app_av_event_to_str(event),
             event);

    switch (s_a2d_state) {
    case APP_AV_STATE_DISCOVERING:
    case APP_AV_STATE_DISCOVERED:
        break;
    case APP_AV_STATE_UNCONNECTED:
        bt_app_av_state_unconnected_hdlr(event, param);
        break;
    case APP_AV_STATE_CONNECTING:
        bt_app_av_state_connecting_hdlr(event, param);
        break;
    case APP_AV_STATE_CONNECTED:
        bt_app_av_state_connected_hdlr(event, param);
        break;
    case APP_AV_STATE_DISCONNECTING:
        bt_app_av_state_disconnecting_hdlr(event, param);
        break;
    default:
        ESP_LOGE(BT_AV_TAG, "%s invalid state: %d", __func__, s_a2d_state);
        break;
    }
}

static const char* bt_app_av_event_to_str(uint16_t event) {
    if (event == BT_APP_MEDIA_START_EVT) {
        return "BT_APP_MEDIA_START_EVT";
    }

    if (event == BT_APP_AUDIO_INFO_EVT) {
        return "BT_APP_AUDIO_INFO_EVT";
    }

    if (event == BT_APP_HEART_BEAT_EVT) {
        return "BT_APP_HEART_BEAT_EVT";
    }

    if (event >= (sizeof(s_a2d_event_names) / sizeof(s_a2d_event_names[0])) || s_a2d_event_names[event] == NULL) {
        return "UNKNOWN";
    }

    return s_a2d_event_names[event];
}

static const char* bt_app_av_state_to_str(int state) {
    if (state < 0 || state >= (int)(sizeof(s_av_state_names) / sizeof(s_av_state_names[0])) ||
        s_av_state_names[state] == NULL) {
        return "UNKNOWN";
    }

    return s_av_state_names[state];
}

static bool is_one_bit_set_u8(uint8_t v) {
    return (v != 0) && ((v & (uint8_t)(v - 1)) == 0);
}

static bool check_pref_mcc_against_sink_caps(const esp_a2d_mcc_t* sink_caps, const esp_a2d_mcc_t* pref_mcc) {
    const esp_a2d_cie_sbc_t* caps;
    const esp_a2d_cie_sbc_t* cfg;

    if (sink_caps == NULL || pref_mcc == NULL) {
        return false;
    }
    if (sink_caps->type != pref_mcc->type) {
        return false;
    }
    if (pref_mcc->type != ESP_A2D_MCT_SBC) {
        return false;
    }

    caps = &sink_caps->cie.sbc_info;
    cfg  = &pref_mcc->cie.sbc_info;

    if (!is_one_bit_set_u8(cfg->samp_freq) || ((cfg->samp_freq & caps->samp_freq) != cfg->samp_freq)) {
        return false;
    }
    if (!is_one_bit_set_u8(cfg->ch_mode) || ((cfg->ch_mode & caps->ch_mode) != cfg->ch_mode)) {
        return false;
    }
    if (!is_one_bit_set_u8(cfg->block_len) || ((cfg->block_len & caps->block_len) != cfg->block_len)) {
        return false;
    }
    if (!is_one_bit_set_u8(cfg->num_subbands) || ((cfg->num_subbands & caps->num_subbands) != cfg->num_subbands)) {
        return false;
    }
    if (!is_one_bit_set_u8(cfg->alloc_mthd) || ((cfg->alloc_mthd & caps->alloc_mthd) != cfg->alloc_mthd)) {
        return false;
    }
    if (cfg->min_bitpool < caps->min_bitpool || cfg->max_bitpool > caps->max_bitpool ||
        cfg->min_bitpool > cfg->max_bitpool) {
        return false;
    }

    return true;
}

static bool bt_app_a2d_sbc_freq_from_sample_rate(uint32_t sample_rate, uint8_t* freq) {
    if (freq == NULL) {
        return false;
    }

    switch (sample_rate) {
    case 16000:
        *freq = ESP_A2D_SBC_CIE_SF_16K;
        return true;
    case 32000:
        *freq = ESP_A2D_SBC_CIE_SF_32K;
        return true;
    case 44100:
        *freq = ESP_A2D_SBC_CIE_SF_44K;
        return true;
    case 48000:
        *freq = ESP_A2D_SBC_CIE_SF_48K;
        return true;
    default:
        return false;
    }
}

static bool bt_app_a2d_sbc_ch_mode_from_channels(const esp_a2d_cie_sbc_t* caps, uint8_t channels, uint8_t* ch_mode) {
    if (caps == NULL || ch_mode == NULL) {
        return false;
    }

    if (channels == 1) {
        if (caps->ch_mode & ESP_A2D_SBC_CIE_CH_MODE_MONO) {
            *ch_mode = ESP_A2D_SBC_CIE_CH_MODE_MONO;
            return true;
        }
    }

    if (channels == 2) {
        if (caps->ch_mode & ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO) {
            *ch_mode = ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO;
            return true;
        }
        if (caps->ch_mode & ESP_A2D_SBC_CIE_CH_MODE_STEREO) {
            *ch_mode = ESP_A2D_SBC_CIE_CH_MODE_STEREO;
            return true;
        }
        if (caps->ch_mode & ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL) {
            *ch_mode = ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL;
            return true;
        }
    }

    return false;
}

static void bt_app_a2d_store_sink_caps(esp_a2d_conn_hdl_t conn_hdl, const esp_a2d_mcc_t* sink_caps) {
    if (sink_caps == NULL) {
        return;
    }

    s_sink_conn_hdl   = conn_hdl;
    s_sink_caps       = *sink_caps;
    s_sink_caps_valid = true;
}

static void bt_app_a2d_store_audio_info(const bt_app_audio_info_t* info) {
    if (info == NULL) {
        return;
    }

    s_audio_info = *info;

    ESP_LOGI(BT_AV_TAG,
             "Decoded audio info received: %lu Hz, %u bits, %u channel(s)",
             s_audio_info.sample_rate,
             s_audio_info.bits_per_sample,
             s_audio_info.channels);

    if (!s_sink_caps_valid) {
        ESP_LOGW(BT_AV_TAG, "Cannot configure A2DP codec yet: sink capabilities not available");
        return;
    }

    bt_app_a2d_set_pref_mcc(s_sink_conn_hdl, &s_sink_caps, &s_audio_info);
}

static void bt_app_a2d_set_pref_mcc(esp_a2d_conn_hdl_t conn_hdl, const esp_a2d_mcc_t* sink_caps,
                                    const bt_app_audio_info_t* audio_info) {
    bt_log_enter(__func__);
    esp_a2d_mcc_t pref_mcc;
    uint8_t samp_freq;
    uint8_t ch_mode;

    if (sink_caps == NULL || audio_info == NULL) {
        ESP_LOGW(BT_AV_TAG, "Cannot set pref_mcc without sink caps and decoded audio info");
        return;
    }
    if (sink_caps->type != ESP_A2D_MCT_SBC) {
        ESP_LOGW(BT_AV_TAG, "Cannot set pref_mcc for non-SBC sink codec type: %d", sink_caps->type);
        return;
    }
    if (audio_info->bits_per_sample != 16) {
        ESP_LOGW(BT_AV_TAG, "Unsupported decoded PCM bit depth for A2DP: %u", audio_info->bits_per_sample);
        return;
    }
    if (!bt_app_a2d_sbc_freq_from_sample_rate(audio_info->sample_rate, &samp_freq)) {
        ESP_LOGW(BT_AV_TAG, "Unsupported decoded PCM sample rate for SBC: %lu", audio_info->sample_rate);
        return;
    }
    if (!bt_app_a2d_sbc_ch_mode_from_channels(&sink_caps->cie.sbc_info, audio_info->channels, &ch_mode)) {
        ESP_LOGW(BT_AV_TAG, "Unsupported decoded PCM channel count for sink: %u", audio_info->channels);
        return;
    }

    memset(&pref_mcc, 0, sizeof(pref_mcc));
    pref_mcc.type                      = ESP_A2D_MCT_SBC;
    pref_mcc.cie.sbc_info.samp_freq    = samp_freq;
    pref_mcc.cie.sbc_info.ch_mode      = ch_mode;
    pref_mcc.cie.sbc_info.block_len    = ESP_A2D_SBC_CIE_BLOCK_LEN_16;
    pref_mcc.cie.sbc_info.num_subbands = ESP_A2D_SBC_CIE_NUM_SUBBANDS_8;
    pref_mcc.cie.sbc_info.alloc_mthd   = ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS;
    pref_mcc.cie.sbc_info.min_bitpool  = sink_caps->cie.sbc_info.min_bitpool;
    pref_mcc.cie.sbc_info.max_bitpool  = sink_caps->cie.sbc_info.max_bitpool;

    if (!check_pref_mcc_against_sink_caps(sink_caps, &pref_mcc)) {
        ESP_LOGW(BT_AV_TAG, "pref_mcc not supported by sink");
        return;
    }

    esp_err_t ret = esp_a2d_source_set_pref_mcc(conn_hdl, &pref_mcc);
    ESP_LOGI(BT_AV_TAG,
             "Set pref_mcc from decoded audio: %lu Hz, %u bits, %u channel(s), result: %s",
             audio_info->sample_rate,
             audio_info->bits_per_sample,
             audio_info->channels,
             esp_err_to_name(ret));
    bt_log_leave(__func__);
}

static void bt_app_a2d_handle_disconnected(void) {
    s_media_state     = APP_AV_MEDIA_STATE_IDLE;
    s_sink_caps_valid = false;
    player_clear();
    screen_notify_show_bt_discovery();

    s_a2d_state   = APP_AV_STATE_DISCOVERING;
    esp_err_t ret = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(BT_AV_TAG, "Failed to restart device discovery after disconnect: %s", esp_err_to_name(ret));
    }
}

static void bt_app_av_state_unconnected_hdlr(uint16_t event, void* param) {
    bt_log_enter(__func__);
    esp_a2d_cb_param_t* a2d = NULL;

    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
    case ESP_A2D_AUDIO_STATE_EVT:
    case ESP_A2D_AUDIO_CFG_EVT:
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
    case BT_APP_MEDIA_START_EVT:
    case BT_APP_AUDIO_INFO_EVT:
        break;
    case BT_APP_HEART_BEAT_EVT: {
        uint8_t* bda = s_peer_bda;
        ESP_LOGI(BT_AV_TAG,
                 "a2dp connecting to peer: %02x:%02x:%02x:%02x:%02x:%02x",
                 bda[0],
                 bda[1],
                 bda[2],
                 bda[3],
                 bda[4],
                 bda[5]);
        esp_a2d_source_connect(s_peer_bda);
        s_a2d_state       = APP_AV_STATE_CONNECTING;
        s_connecting_intv = 0;
        break;
    }
    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT:
        a2d = (esp_a2d_cb_param_t*)(param);
        ESP_LOGI(BT_AV_TAG, "%s, delay value: %u * 1/10 ms", __func__, a2d->a2d_report_delay_value_stat.delay_value);
        break;
    default:
        ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
    bt_log_leave(__func__);
}

static void bt_app_av_state_connecting_hdlr(uint16_t event, void* param) {
    bt_log_enter(__func__);
    esp_a2d_cb_param_t* a2d = NULL;

    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        a2d = (esp_a2d_cb_param_t*)(param);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(BT_AV_TAG, "a2dp connected");
            s_a2d_state   = APP_AV_STATE_CONNECTED;
            s_media_state = APP_AV_MEDIA_STATE_IDLE;
            screen_notify_show_song_selection();
        } else if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            bt_app_a2d_handle_disconnected();
        }
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
    case ESP_A2D_AUDIO_CFG_EVT:
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
    case BT_APP_MEDIA_START_EVT:
    case BT_APP_AUDIO_INFO_EVT:
        break;
    case BT_APP_HEART_BEAT_EVT:
        if (++s_connecting_intv >= 2) {
            s_a2d_state       = APP_AV_STATE_UNCONNECTED;
            s_connecting_intv = 0;
        }
        break;
    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT:
        a2d = (esp_a2d_cb_param_t*)(param);
        ESP_LOGI(BT_AV_TAG, "%s, delay value: %u * 1/10 ms", __func__, a2d->a2d_report_delay_value_stat.delay_value);
        break;
    default:
        ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
    bt_log_leave(__func__);
}

static void bt_app_av_media_proc(uint16_t event, void* param) {
    bt_log_enter(__func__);
    esp_a2d_cb_param_t* a2d = NULL;

    switch (s_media_state) {
    case APP_AV_MEDIA_STATE_IDLE:
        if (event == BT_APP_MEDIA_START_EVT) {
            ESP_LOGI(BT_AV_TAG, "a2dp media ready checking ...");
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        } else if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
            a2d = (esp_a2d_cb_param_t*)(param);
            if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY &&
                a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                ESP_LOGI(BT_AV_TAG, "a2dp media ready, starting ...");
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
                s_media_state = APP_AV_MEDIA_STATE_STARTING;
            }
        }
        break;
    case APP_AV_MEDIA_STATE_STARTING:
        if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
            a2d = (esp_a2d_cb_param_t*)(param);
            if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_START &&
                a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                ESP_LOGI(BT_AV_TAG, "a2dp media start successfully.");
                s_media_state = APP_AV_MEDIA_STATE_STARTED;
            } else {
                ESP_LOGI(BT_AV_TAG, "a2dp media start failed.");
                s_media_state = APP_AV_MEDIA_STATE_IDLE;
            }
        }
        break;
    case APP_AV_MEDIA_STATE_STARTED:
        break;
    case APP_AV_MEDIA_STATE_STOPPING:
        if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
            a2d = (esp_a2d_cb_param_t*)(param);
            if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_SUSPEND &&
                a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                ESP_LOGI(BT_AV_TAG, "a2dp media suspend successfully, disconnecting...");
                s_media_state = APP_AV_MEDIA_STATE_IDLE;
                esp_a2d_source_disconnect(s_peer_bda);
                s_a2d_state = APP_AV_STATE_DISCONNECTING;
            } else {
                ESP_LOGI(BT_AV_TAG, "a2dp media suspending...");
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
            }
        }
        break;
    default:
        break;
    }
    bt_log_leave(__func__);
}

static void bt_app_av_state_connected_hdlr(uint16_t event, void* param) {
    esp_a2d_cb_param_t* a2d = NULL;

    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        a2d = (esp_a2d_cb_param_t*)(param);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(BT_AV_TAG, "a2dp disconnected");
            bt_app_a2d_handle_disconnected();
        }
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        a2d = (esp_a2d_cb_param_t*)(param);
        if (ESP_A2D_AUDIO_STATE_STARTED == a2d->audio_stat.state) {
            s_pkt_cnt = 0;
        }
        break;
    case ESP_A2D_AUDIO_CFG_EVT:
        break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
    case BT_APP_MEDIA_START_EVT:
        bt_app_av_media_proc(event, param);
        break;
    case BT_APP_AUDIO_INFO_EVT:
        bt_app_a2d_store_audio_info((const bt_app_audio_info_t*)param);
        break;
    case BT_APP_HEART_BEAT_EVT:
        break;
    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT:
        a2d = (esp_a2d_cb_param_t*)(param);
        ESP_LOGI(BT_AV_TAG, "%s, delay value: %u * 1/10 ms", __func__, a2d->a2d_report_delay_value_stat.delay_value);
        break;
    case ESP_A2D_REPORT_SNK_CODEC_CAPS_EVT: {
        a2d                     = (esp_a2d_cb_param_t*)(param);
        esp_a2d_mcc_t* sink_mcc = &a2d->a2d_report_snk_codec_caps_stat.mcc;
        ESP_LOGI(BT_AV_TAG, "sink codec type: %d", sink_mcc->type);
        if (sink_mcc->type == ESP_A2D_MCT_SBC) {
            ESP_LOGI(BT_AV_TAG,
                     "sink codec capabilities: 0x%x-0x%x-0x%x-0x%x-0x%x-%d-%d",
                     sink_mcc->cie.sbc_info.samp_freq,
                     sink_mcc->cie.sbc_info.ch_mode,
                     sink_mcc->cie.sbc_info.block_len,
                     sink_mcc->cie.sbc_info.num_subbands,
                     sink_mcc->cie.sbc_info.alloc_mthd,
                     sink_mcc->cie.sbc_info.min_bitpool,
                     sink_mcc->cie.sbc_info.max_bitpool);
        }
        bt_app_a2d_store_sink_caps(a2d->a2d_report_snk_codec_caps_stat.conn_hdl, sink_mcc);
        break;
    }
    case ESP_A2D_SRC_SET_PREF_MCC_EVT:
        a2d = (esp_a2d_cb_param_t*)(param);
        ESP_LOGI(BT_AV_TAG,
                 "Set preferred media codec config result: conn_hdl: %d, set_status: %d",
                 a2d->a2d_set_pref_mcc_stat.conn_hdl,
                 a2d->a2d_set_pref_mcc_stat.set_status);
        break;
    default:
        ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
}

static void bt_app_av_state_disconnecting_hdlr(uint16_t event, void* param) {
    esp_a2d_cb_param_t* a2d = NULL;

    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        a2d = (esp_a2d_cb_param_t*)(param);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(BT_AV_TAG, "a2dp disconnected");
            bt_app_a2d_handle_disconnected();
        }
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
    case ESP_A2D_AUDIO_CFG_EVT:
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
    case BT_APP_MEDIA_START_EVT:
    case BT_APP_HEART_BEAT_EVT:
    case BT_APP_AUDIO_INFO_EVT:
        break;
    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT:
        a2d = (esp_a2d_cb_param_t*)(param);
        ESP_LOGI(BT_AV_TAG, "%s, delay value: 0x%u * 1/10 ms", __func__, a2d->a2d_report_delay_value_stat.delay_value);
        break;
    default:
        ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
}
