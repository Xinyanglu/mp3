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
#define PLAYER_PCM_WRITE_LOG_INTERVAL_BYTES (64 * 1024)

static const char* TAG = "player";
static StreamBufferHandle_t pcm_stream;
static size_t pcm_total_written;
static size_t pcm_next_write_log;
static uint64_t pcm_total_read;
static uint32_t pcm_bytes_per_second;
static uint32_t pcm_total_seconds;
static uint32_t pcm_last_progress_seconds;
static bool a2dp_start_requested;
static bool playback_screen_notified;

typedef enum {
    PLAYER_EVT_PLAY,
    PLAYER_EVT_PAUSE,
    PLAYER_EVT_RESUME,
    PLAYER_EVT_A2DP_STARTED,
    PLAYER_EVT_FINISHED,
    PLAYER_EVT_CLEAR,
} player_event_t;

typedef struct {
    player_event_t event;
    char path[SDCARD_MAX_PATH_LEN];
} player_msg_t;

typedef struct {
    player_audio_info_t* info;
    uint64_t file_size_bytes;
} player_decode_ctx_t;

static QueueHandle_t player_queue;
static TaskHandle_t player_task_handle;
static esp_asp_handle_t active_simple_player;
static char active_song_path[SDCARD_MAX_PATH_LEN];
static player_audio_info_t active_audio_info;
static player_decode_ctx_t active_decode_ctx = {
    .info = &active_audio_info,
};

static void player_task_handler(void* arg);
static esp_err_t player_send_msg(const player_msg_t* msg);
static void player_handle_play(const char* path);
static void player_handle_pause(void);
static void player_handle_resume(void);
static void player_handle_a2dp_started(void);
static void player_handle_finished(void);
static void player_handle_clear(void);
static void player_clear_pending_events(void);
static void player_reset_pcm_state(void);
static void player_destroy_active(void);
static esp_err_t player_build_file_uri(char* uri, size_t uri_size, const char* path);
static esp_err_t player_gmf_err_to_esp_err(esp_gmf_err_t err);
static esp_err_t player_start_file(const char* path);
static uint32_t player_estimate_total_seconds(uint64_t file_size_bytes, uint32_t bitrate);

static int player_simple_out_cb(uint8_t* data, int data_size, void* ctx) {
    player_decode_ctx_t* decode_ctx = (player_decode_ctx_t*)ctx;
    size_t written                  = 0;
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

    return 0;
}

static int player_simple_event_cb(esp_asp_event_pkt_t* event, void* ctx) {
    player_decode_ctx_t* decode_ctx = (player_decode_ctx_t*)ctx;
    esp_asp_music_info_t music_info = {0};
    esp_asp_state_t state;

    if (decode_ctx == NULL || event == NULL) {
        return 0;
    }

    if (event->type == ESP_ASP_EVENT_TYPE_STATE) {
        if (event->payload == NULL || event->payload_size < (int)sizeof(state)) {
            return 0;
        }

        memcpy(&state, event->payload, sizeof(state));
        ESP_LOGI(TAG, "Simple player state: %s", esp_audio_simple_player_state_to_str(state));
        if (state == ESP_ASP_STATE_FINISHED) {
            player_msg_t msg = {
                .event = PLAYER_EVT_FINISHED,
            };
            if (player_send_msg(&msg) != ESP_OK) {
                ESP_LOGW(TAG, "Failed to enqueue player finished event");
            }
        }
        return 0;
    }

    if (event->type != ESP_ASP_EVENT_TYPE_MUSIC_INFO || decode_ctx->info == NULL || event->payload == NULL ||
        event->payload_size < (int)sizeof(music_info)) {
        return 0;
    }

    memcpy(&music_info, event->payload, sizeof(music_info));
    decode_ctx->info->sample_rate     = (uint32_t)music_info.sample_rate;
    decode_ctx->info->bits_per_sample = music_info.bits;
    decode_ctx->info->channels        = music_info.channels;
    decode_ctx->info->bitrate         = (uint32_t)music_info.bitrate;

    pcm_bytes_per_second =
        decode_ctx->info->sample_rate * decode_ctx->info->channels * (decode_ctx->info->bits_per_sample / 8U);
    pcm_total_seconds         = player_estimate_total_seconds(decode_ctx->file_size_bytes, decode_ctx->info->bitrate);
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

    if (!a2dp_start_requested) {
        a2dp_start_requested = true;
        ret = bt_app_start_media();
        if (ret != ESP_OK) {
            a2dp_start_requested = false;
            ESP_LOGW(TAG, "Failed to request A2DP media start: %s", esp_err_to_name(ret));
        }
    }

    return 0;
}

esp_err_t player_init(void) {
    if (player_queue != NULL) {
        return ESP_OK;
    }

    player_queue = xQueueCreate(PLAYER_QUEUE_LEN, sizeof(player_msg_t));
    if (player_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    pcm_stream = xStreamBufferCreateWithCaps(PLAYER_PCM_STREAM_BUFFER_SIZE, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pcm_stream == NULL) {
        vQueueDelete(player_queue);
        player_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_created = xTaskCreate(
        player_task_handler, "PlayerTask", PLAYER_TASK_STACK_SIZE, NULL, PLAYER_TASK_PRIORITY, &player_task_handle);
    if (task_created != pdPASS) {
        vStreamBufferDeleteWithCaps(pcm_stream);
        pcm_stream = NULL;
        vQueueDelete(player_queue);
        player_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t player_play(size_t song_idx) {
    player_msg_t msg = {
        .event = PLAYER_EVT_PLAY,
    };

    ESP_RETURN_ON_FALSE(player_queue != NULL, ESP_ERR_INVALID_STATE, TAG, "Player queue not initialized");

    ESP_RETURN_ON_ERROR(sdcard_get_song_path(song_idx, msg.path, sizeof(msg.path)), TAG, "Invalid song path");

    return player_send_msg(&msg);
}

esp_err_t player_pause(void) {
    player_msg_t msg = {
        .event = PLAYER_EVT_PAUSE,
    };

    ESP_RETURN_ON_FALSE(player_queue != NULL, ESP_ERR_INVALID_STATE, TAG, "Player queue not initialized");

    return player_send_msg(&msg);
}

esp_err_t player_resume(void) {
    player_msg_t msg = {
        .event = PLAYER_EVT_RESUME,
    };

    ESP_RETURN_ON_FALSE(player_queue != NULL, ESP_ERR_INVALID_STATE, TAG, "Player queue not initialized");

    return player_send_msg(&msg);
}

esp_err_t player_clear(void) {
    player_msg_t msg = {
        .event = PLAYER_EVT_CLEAR,
    };

    ESP_RETURN_ON_FALSE(player_queue != NULL, ESP_ERR_INVALID_STATE, TAG, "Player queue not initialized");

    return player_send_msg(&msg);
}

static esp_err_t player_start_file(const char* path) {
    esp_asp_handle_t simple_player = NULL;
    char uri[PLAYER_MAX_FILE_URI_LEN];
    struct stat st;
    esp_asp_cfg_t player_cfg = {
        .out.cb            = player_simple_out_cb,
        .out.user_ctx      = &active_decode_ctx,
        .task_prio         = PLAYER_SIMPLE_TASK_PRIORITY,
        .task_stack        = PLAYER_SIMPLE_TASK_STACK_SIZE,
        .task_core         = PLAYER_SIMPLE_TASK_CORE,
        .task_stack_in_ext = true,
    };
    esp_gmf_err_t gmf_ret;
    esp_gmf_err_t destroy_ret;
    esp_err_t ret = ESP_OK;

    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    player_destroy_active();
    memset(&active_audio_info, 0, sizeof(active_audio_info));
    active_decode_ctx.file_size_bytes = 0;

    if (stat(path, &st) == 0 && st.st_size > 0) {
        active_decode_ctx.file_size_bytes = (uint64_t)st.st_size;
    }

    ret = player_build_file_uri(uri, sizeof(uri), path);
    if (ret != ESP_OK) {
        return ret;
    }

    gmf_ret = esp_audio_simple_player_new(&player_cfg, &simple_player);
    if (gmf_ret != ESP_GMF_ERR_OK) {
        return player_gmf_err_to_esp_err(gmf_ret);
    }

    gmf_ret = esp_audio_simple_player_set_event(simple_player, player_simple_event_cb, &active_decode_ctx);
    if (gmf_ret == ESP_GMF_ERR_OK) {
        gmf_ret = esp_audio_simple_player_run(simple_player, uri, NULL);
    }

    if (gmf_ret != ESP_GMF_ERR_OK) {
        ret         = player_gmf_err_to_esp_err(gmf_ret);
        destroy_ret = esp_audio_simple_player_destroy(simple_player);
        if (ret == ESP_OK && destroy_ret != ESP_GMF_ERR_OK) {
            ret = player_gmf_err_to_esp_err(destroy_ret);
        }
        return ret;
    }

    active_simple_player = simple_player;

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

    if (a2dp_start_requested && pcm_bytes_per_second > 0 && bytes_read > 0) {
        pcm_total_read += bytes_read;
        elapsed_seconds = (uint32_t)(pcm_total_read / pcm_bytes_per_second);
        if (elapsed_seconds != pcm_last_progress_seconds) {
            pcm_last_progress_seconds = elapsed_seconds;
            screen_notify_song_progress(elapsed_seconds, pcm_total_seconds);
        }
    }

    return len;
}

void player_notify_a2dp_started(void) {
    player_msg_t msg = {
        .event = PLAYER_EVT_A2DP_STARTED,
    };

    if (player_send_msg(&msg) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enqueue A2DP started event");
    }
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
        case PLAYER_EVT_PAUSE:
            player_handle_pause();
            break;
        case PLAYER_EVT_RESUME:
            player_handle_resume();
            break;
        case PLAYER_EVT_A2DP_STARTED:
            player_handle_a2dp_started();
            break;
        case PLAYER_EVT_FINISHED:
            player_handle_finished();
            break;
        case PLAYER_EVT_CLEAR:
            player_handle_clear();
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
    esp_err_t ret;

    if (path == NULL || path[0] == '\0') {
        return;
    }

    strlcpy(play_path, path, sizeof(play_path));

    player_reset_pcm_state();

    ESP_LOGI(TAG, "Playing song: %s", play_path);
    ret = player_start_file(play_path);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to play song %s: %s", play_path, esp_err_to_name(ret));
        active_song_path[0] = '\0';
        return;
    }

    strlcpy(active_song_path, play_path, sizeof(active_song_path));
}

static void player_handle_pause(void) {
    esp_gmf_err_t gmf_ret;

    if (active_simple_player == NULL) {
        ESP_LOGW(TAG, "Cannot pause; no active player");
        return;
    }

    gmf_ret = esp_audio_simple_player_pause(active_simple_player);
    if (gmf_ret != ESP_GMF_ERR_OK) {
        ESP_LOGW(TAG, "Failed to pause player: %s", esp_err_to_name(player_gmf_err_to_esp_err(gmf_ret)));
    }
}

static void player_handle_resume(void) {
    esp_gmf_err_t gmf_ret;

    if (active_simple_player == NULL) {
        ESP_LOGW(TAG, "Cannot resume; no active player");
        return;
    }

    gmf_ret = esp_audio_simple_player_resume(active_simple_player);
    if (gmf_ret != ESP_GMF_ERR_OK) {
        ESP_LOGW(TAG, "Failed to resume player: %s", esp_err_to_name(player_gmf_err_to_esp_err(gmf_ret)));
    }
}

static void player_handle_a2dp_started(void) {
    uint32_t elapsed_seconds = 0;

    if (!a2dp_start_requested || playback_screen_notified) {
        return;
    }

    playback_screen_notified = true;
    if (pcm_bytes_per_second > 0) {
        elapsed_seconds = (uint32_t)(pcm_total_read / pcm_bytes_per_second);
    }

    screen_notify_show_song_playing();
    screen_notify_song_progress(elapsed_seconds, pcm_total_seconds);
}

static void player_handle_finished(void) {
    player_handle_clear();
    screen_notify_song_finished();
}

static void player_handle_clear(void) {
    player_clear_pending_events();
    player_destroy_active();
    player_reset_pcm_state();
    active_song_path[0] = '\0';
}

static void player_clear_pending_events(void) {
    if (player_queue != NULL) {
        xQueueReset(player_queue);
    }
}

static void player_reset_pcm_state(void) {
    if (pcm_stream != NULL) {
        xStreamBufferReset(pcm_stream);
    }
    pcm_total_written         = 0;
    pcm_total_read            = 0;
    pcm_next_write_log        = 0;
    pcm_bytes_per_second      = 0;
    pcm_total_seconds         = 0;
    pcm_last_progress_seconds = 0;
    a2dp_start_requested      = false;
    playback_screen_notified  = false;
}

static void player_destroy_active(void) {
    esp_gmf_err_t gmf_ret;

    if (active_simple_player == NULL) {
        return;
    }

    gmf_ret = esp_audio_simple_player_destroy(active_simple_player);
    if (gmf_ret != ESP_GMF_ERR_OK) {
        ESP_LOGW(TAG, "Failed to destroy active player: %s", esp_err_to_name(player_gmf_err_to_esp_err(gmf_ret)));
    }
    active_simple_player = NULL;
}

static uint32_t player_estimate_total_seconds(uint64_t file_size_bytes, uint32_t bitrate_kbps) {
    uint64_t total_bits;
    uint32_t bitrate_bps;

    if (file_size_bytes == 0 || bitrate_kbps == 0) {
        return 0;
    }

    bitrate_bps = bitrate_kbps * 1000U;
    total_bits  = file_size_bytes * 8U;
    return (uint32_t)((total_bits + bitrate_bps - 1U) / bitrate_bps);
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

    written =
        snprintf(uri, uri_size, PLAYER_FILE_URI_PREFIX "%.*s%s", (int)(path_after_host - host), host, path_after_host);
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
