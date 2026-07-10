/*
 * Minimal playback state owner.
 *
 * This module owns current-song playback state and is the handoff point between
 * UI song selection and the A2DP audio callback.
 */
#include "player.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "bt_app.h"
#include "esp_audio_simple_player.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "portmacro.h"
#include "screen.h"

#define PLAYER_QUEUE_LEN 10
#define PLAYER_TASK_STACK_SIZE 4096
#define PLAYER_TASK_PRIORITY 10
#define PLAYER_SIMPLE_TASK_STACK_SIZE 6144
#define PLAYER_SIMPLE_TASK_PRIORITY 12
#define PLAYER_SIMPLE_TASK_CORE 1
#define PLAYER_FILE_URI_PREFIX "file://"
#define PLAYER_MAX_FILE_URI_LEN (sizeof(PLAYER_FILE_URI_PREFIX) + SDCARD_MAX_PATH_LEN)
#define PLAYER_PCM_STREAM_BUFFER_SIZE (32 * 1024)
#define PLAYER_PCM_PRIME_BYTES (6 * 1024)
#define PLAYER_PCM_WRITE_LOG_INTERVAL_BYTES (64 * 1024)
#define PLAYER_PCM_GAIN_NUMERATOR 1
#define PLAYER_PCM_GAIN_DENOMINATOR 8

static const char* TAG = "player";
static StreamBufferHandle_t pcm_stream;
static size_t pcm_total_written;
static size_t pcm_next_write_log;
static uint64_t pcm_total_read;
static uint32_t pcm_bytes_per_second;
static uint32_t pcm_total_seconds;
static uint32_t pcm_last_progress_seconds;
static bool pcm_media_started;

typedef enum {
    PLAYER_EVT_PLAY,
} player_event_t;

typedef struct {
    player_event_t event;
    char path[SDCARD_MAX_PATH_LEN];
} player_msg_t;

typedef struct {
    player_audio_info_t* info;
    uint64_t file_size_bytes;
} player_decode_ctx_t;

static SemaphoreHandle_t player_lock;
static QueueHandle_t player_queue;
static TaskHandle_t player_task_handle;
static player_status_t player_status = {
    .state      = PLAYER_STATE_STOPPED,
    .path       = "",
    .last_error = ESP_OK,
};

static void player_task_handler(void* arg);
static esp_err_t player_send_msg(const player_msg_t* msg);
static void player_handle_play(const char* path);
static void player_apply_pcm_gain(uint8_t* data, size_t len);
static esp_err_t player_build_file_uri(char* uri, size_t uri_size, const char* path);
static esp_err_t player_gmf_err_to_esp_err(esp_gmf_err_t err);
static esp_err_t player_decode_file(const char* path, player_audio_info_t* info);
static uint32_t player_estimate_total_seconds(uint64_t file_size_bytes, uint32_t bitrate);

static int player_simple_out_cb(uint8_t* data, int data_size, void* ctx) {
    player_decode_ctx_t* decode_ctx = (player_decode_ctx_t*)ctx;
    size_t written = 0;
    size_t data_len;

    if (decode_ctx == NULL || data == NULL || data_size < 0 || pcm_stream == NULL) {
        return ESP_FAIL;
    }

    data_len = (size_t)data_size;
    if (data_len == 0) {
        return 0;
    }

    if (pcm_next_write_log == 0) {
        pcm_next_write_log = PLAYER_PCM_WRITE_LOG_INTERVAL_BYTES;
        ESP_LOGI(TAG, "First PCM output callback: %u bytes", (unsigned)data_len);
    }

    player_apply_pcm_gain(data, data_len);

    while (written < data_len) {
        size_t sent = xStreamBufferSend(pcm_stream, data + written, data_len - written, portMAX_DELAY);
        if (sent == 0) {
            return ESP_FAIL;
        }
        written += sent;
    }

    pcm_total_written += written;
    if (pcm_total_written >= pcm_next_write_log) {
        size_t buffered = xStreamBufferBytesAvailable(pcm_stream);
        ESP_LOGI(TAG, "PCM written: %u total, buffered %u", (unsigned)pcm_total_written, (unsigned)buffered);
        pcm_next_write_log += PLAYER_PCM_WRITE_LOG_INTERVAL_BYTES;
    }

    if (!pcm_media_started) {
        size_t buffered = xStreamBufferBytesAvailable(pcm_stream);
        if (buffered >= PLAYER_PCM_PRIME_BYTES) {
            ESP_LOGI(TAG, "PCM primed: %u buffered, starting A2DP media", (unsigned)buffered);
            pcm_media_started = true;
            screen_notify_show_song_playing();
            screen_notify_song_progress(0, pcm_total_seconds);
            bt_app_start_media();
        }
    }

    return 0;
}

static int player_simple_event_cb(esp_asp_event_pkt_t* event, void* ctx) {
    player_decode_ctx_t* decode_ctx = (player_decode_ctx_t*)ctx;
    esp_asp_music_info_t music_info = {0};

    if (decode_ctx == NULL || event == NULL || event->type != ESP_ASP_EVENT_TYPE_MUSIC_INFO || decode_ctx->info == NULL ||
        event->payload == NULL || event->payload_size < (int)sizeof(music_info)) {
        return 0;
    }

    memcpy(&music_info, event->payload, sizeof(music_info));
    decode_ctx->info->sample_rate = (uint32_t)music_info.sample_rate;
    decode_ctx->info->bits_per_sample = music_info.bits;
    decode_ctx->info->channels = music_info.channels;
    decode_ctx->info->bitrate = (uint32_t)music_info.bitrate;

    pcm_bytes_per_second = decode_ctx->info->sample_rate * decode_ctx->info->channels * (decode_ctx->info->bits_per_sample / 8U);
    pcm_total_seconds = player_estimate_total_seconds(decode_ctx->file_size_bytes, decode_ctx->info->bitrate);
    pcm_last_progress_seconds = 0;

    ESP_LOGI(TAG,
             "Decoded audio info: %lu Hz, %u bits, %u channel(s), bitrate %lu, estimated duration %lu second(s)",
             decode_ctx->info->sample_rate,
             decode_ctx->info->bits_per_sample,
             decode_ctx->info->channels,
             decode_ctx->info->bitrate,
             pcm_total_seconds);

    bt_app_audio_info_t bt_audio_info = {
        .sample_rate     = decode_ctx->info->sample_rate,
        .bits_per_sample = decode_ctx->info->bits_per_sample,
        .channels        = decode_ctx->info->channels,
    };
    esp_err_t ret = bt_app_set_audio_info(&bt_audio_info);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to dispatch decoded audio info to BT app: %s", esp_err_to_name(ret));
    }

    return 0;
}

esp_err_t player_init(void) {
    if (player_lock != NULL) {
        return ESP_OK;
    }

    player_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(player_lock != NULL, ESP_ERR_NO_MEM, TAG, "Player mutex create failed");

    player_queue = xQueueCreate(PLAYER_QUEUE_LEN, sizeof(player_msg_t));
    if (player_queue == NULL) {
        vSemaphoreDelete(player_lock);
        player_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    pcm_stream = xStreamBufferCreateWithCaps(PLAYER_PCM_STREAM_BUFFER_SIZE, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pcm_stream == NULL) {
        vQueueDelete(player_queue);
        player_queue = NULL;
        vSemaphoreDelete(player_lock);
        player_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_created =
        xTaskCreate(player_task_handler, "PlayerTask", PLAYER_TASK_STACK_SIZE, NULL, PLAYER_TASK_PRIORITY, &player_task_handle);
    if (task_created != pdPASS) {
        vStreamBufferDeleteWithCaps(pcm_stream);
        pcm_stream = NULL;
        vQueueDelete(player_queue);
        player_queue = NULL;
        vSemaphoreDelete(player_lock);
        player_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t player_play(size_t song_idx) {
    player_msg_t msg = {
        .event = PLAYER_EVT_PLAY,
    };

    ESP_RETURN_ON_FALSE(player_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "Player not initialized");
    ESP_RETURN_ON_FALSE(player_queue != NULL, ESP_ERR_INVALID_STATE, TAG, "Player queue not initialized");

    ESP_RETURN_ON_ERROR(sdcard_get_song_path(song_idx, msg.path, sizeof(msg.path)), TAG, "Invalid song path");

    return player_send_msg(&msg);
}

esp_err_t player_pause(void) {
    ESP_RETURN_ON_FALSE(player_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "Player not initialized");

    if (xSemaphoreTake(player_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    if (player_status.state == PLAYER_STATE_PLAYING) {
        player_status.state = PLAYER_STATE_PAUSED;
    }

    xSemaphoreGive(player_lock);
    return ESP_OK;
}

esp_err_t player_stop(void) {
    ESP_RETURN_ON_FALSE(player_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "Player not initialized");

    if (xSemaphoreTake(player_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    player_status.state      = PLAYER_STATE_STOPPED;
    player_status.path[0]    = '\0';
    player_status.last_error = ESP_OK;

    xSemaphoreGive(player_lock);
    return ESP_OK;
}

esp_err_t player_get_status(player_status_t* status) {
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid status pointer");
    ESP_RETURN_ON_FALSE(player_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "Player not initialized");

    if (xSemaphoreTake(player_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    *status = player_status;

    xSemaphoreGive(player_lock);
    return ESP_OK;
}

static esp_err_t player_decode_file(const char* path, player_audio_info_t* info) {
    esp_asp_handle_t simple_player = NULL;
    char uri[PLAYER_MAX_FILE_URI_LEN];
    struct stat st;
    player_decode_ctx_t decode_ctx = {
        .info = info,
    };
    esp_asp_cfg_t player_cfg = {
        .out.cb = player_simple_out_cb,
        .out.user_ctx = &decode_ctx,
        .task_prio = PLAYER_SIMPLE_TASK_PRIORITY,
        .task_stack = PLAYER_SIMPLE_TASK_STACK_SIZE,
        .task_core = PLAYER_SIMPLE_TASK_CORE,
        .task_stack_in_ext = true,
    };
    esp_gmf_err_t gmf_ret;
    esp_gmf_err_t destroy_ret;
    esp_err_t ret = ESP_OK;

    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (info != NULL) {
        memset(info, 0, sizeof(*info));
    }

    if (stat(path, &st) == 0 && st.st_size > 0) {
        decode_ctx.file_size_bytes = (uint64_t)st.st_size;
    }

    ret = player_build_file_uri(uri, sizeof(uri), path);
    if (ret != ESP_OK) {
        return ret;
    }

    gmf_ret = esp_audio_simple_player_new(&player_cfg, &simple_player);
    if (gmf_ret != ESP_GMF_ERR_OK) {
        return player_gmf_err_to_esp_err(gmf_ret);
    }

    gmf_ret = esp_audio_simple_player_set_event(simple_player, player_simple_event_cb, &decode_ctx);
    if (gmf_ret == ESP_GMF_ERR_OK) {
        gmf_ret = esp_audio_simple_player_run_to_end(simple_player, uri, NULL);
    }

    if (gmf_ret != ESP_GMF_ERR_OK) {
        ret = player_gmf_err_to_esp_err(gmf_ret);
    }

    destroy_ret = esp_audio_simple_player_destroy(simple_player);
    if (ret == ESP_OK && destroy_ret != ESP_GMF_ERR_OK) {
        ret = player_gmf_err_to_esp_err(destroy_ret);
    }

    return ret;
}

int32_t player_read_pcm(uint8_t* data, int32_t len) {
    size_t bytes_read = 0;
    uint32_t elapsed_seconds;

    if (data == NULL || len <= 0) {
        return 0;
    }

    if (pcm_stream != NULL) {
        bytes_read = xStreamBufferReceive(pcm_stream, data, (size_t)len, 0);
    }

    if (bytes_read < (size_t)len) {
        memset(data + bytes_read, 0, (size_t)len - bytes_read);
    }

    if (pcm_media_started && pcm_bytes_per_second > 0) {
        pcm_total_read += (uint32_t)len;
        elapsed_seconds = (uint32_t)(pcm_total_read / pcm_bytes_per_second);
        if (elapsed_seconds != pcm_last_progress_seconds) {
            pcm_last_progress_seconds = elapsed_seconds;
            screen_notify_song_progress(elapsed_seconds, pcm_total_seconds);
        }
    }

    return len;
}

static void player_task_handler(void* arg __attribute__((unused))) {
    player_msg_t msg;

    for (;;) {
        if (xQueueReceive(player_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (msg.event) {
        case PLAYER_EVT_PLAY:
            player_handle_play(msg.path);
            break;
        default:
            ESP_LOGW(TAG, "Unhandled player event: %d", msg.event);
            break;
        }
    }
}

static esp_err_t player_send_msg(const player_msg_t* msg) {
    if (msg == NULL || player_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xQueueSend(player_queue, msg, 10 / portTICK_PERIOD_MS) != pdTRUE) {
        ESP_LOGE(TAG, "Player queue send failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void player_handle_play(const char* path) {
    char play_path[SDCARD_MAX_PATH_LEN];
    player_audio_info_t audio_info = {0};
    esp_err_t ret;

    if (path == NULL || path[0] == '\0') {
        return;
    }

    strlcpy(play_path, path, sizeof(play_path));

    if (xSemaphoreTake(player_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }

    strlcpy(player_status.path, play_path, sizeof(player_status.path));
    player_status.state      = PLAYER_STATE_PLAYING;
    player_status.last_error = ESP_OK;

    xSemaphoreGive(player_lock);

    if (pcm_stream != NULL) {
        xStreamBufferReset(pcm_stream);
    }
    pcm_total_written = 0;
    pcm_total_read = 0;
    pcm_next_write_log = 0;
    pcm_bytes_per_second = 0;
    pcm_total_seconds = 0;
    pcm_last_progress_seconds = 0;
    pcm_media_started = false;

    ESP_LOGI(TAG, "Playing song: %s", play_path);
    ret = player_decode_file(play_path, &audio_info);

    if (xSemaphoreTake(player_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }

    player_status.last_error = ret;
    player_status.state = ret == ESP_OK ? PLAYER_STATE_STOPPED : PLAYER_STATE_ERROR;

    xSemaphoreGive(player_lock);
}

static uint32_t player_estimate_total_seconds(uint64_t file_size_bytes, uint32_t bitrate_kbps) {
    uint64_t total_bits;
    uint32_t bitrate_bps;

    if (file_size_bytes == 0 || bitrate_kbps == 0) {
        return 0;
    }

    bitrate_bps = bitrate_kbps * 1000U;
    total_bits = file_size_bytes * 8U;
    return (uint32_t)((total_bits + bitrate_bps - 1U) / bitrate_bps);
}

static void player_apply_pcm_gain(uint8_t* data, size_t len) {
    int16_t* samples = (int16_t*)data;
    size_t sample_count = len / sizeof(int16_t);

    for (size_t i = 0; i < sample_count; i++) {
        samples[i] = (int16_t)(((int32_t)samples[i] * PLAYER_PCM_GAIN_NUMERATOR) / PLAYER_PCM_GAIN_DENOMINATOR);
    }
}

static esp_err_t player_build_file_uri(char* uri, size_t uri_size, const char* path) {
    const char* host;
    const char* path_after_host;
    int written;

    if (uri == NULL || uri_size == 0 || path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    host = path;
    while (*host == '/') {
        host++;
    }
    if (*host == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    path_after_host = strchr(host, '/');
    if (path_after_host == NULL || path_after_host[1] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    written = snprintf(uri, uri_size, PLAYER_FILE_URI_PREFIX "%.*s%s", (int)(path_after_host - host), host, path_after_host);
    if (written < 0 || (size_t)written >= uri_size) {
        ESP_LOGE(TAG, "Audio file URI too long: %s", path);
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t player_gmf_err_to_esp_err(esp_gmf_err_t err) {
    switch (err) {
    case ESP_GMF_ERR_OK:
        return ESP_OK;
    case ESP_GMF_ERR_INVALID_ARG:
    case ESP_GMF_ERR_INVALID_URI:
    case ESP_GMF_ERR_INVALID_PATH:
        return ESP_ERR_INVALID_ARG;
    case ESP_GMF_ERR_MEMORY_LACK:
        return ESP_ERR_NO_MEM;
    case ESP_GMF_ERR_NOT_SUPPORT:
        return ESP_ERR_NOT_SUPPORTED;
    case ESP_GMF_ERR_NOT_FOUND:
        return ESP_ERR_NOT_FOUND;
    case ESP_GMF_ERR_INVALID_STATE:
    case ESP_GMF_ERR_NOT_READY:
        return ESP_ERR_INVALID_STATE;
    case ESP_GMF_ERR_TIMEOUT:
        return ESP_ERR_TIMEOUT;
    case ESP_GMF_ERR_NOT_ENOUGH:
        return ESP_ERR_INVALID_SIZE;
    default:
        return ESP_FAIL;
    }
}
