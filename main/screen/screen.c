/*
 * Screen state and UI task handling.
 *
 * Owns screen navigation state and forwards drawing to screen_render.
 */
#include "screen.h"

#include <stdbool.h>
#include <string.h>

#include "bt_app.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "player.h"
#include "screen_render.h"
#include "sdcard.h"

#define MAX_BT_DEVICE_NUM 16
#define MAX_BT_DEVICE_TIMEOUT_MS 10000

static const char* TAG = "screen";

static screen_bt_device_t bt_devices[MAX_BT_DEVICE_NUM];
static uint8_t num_bt_devices;
static int selected_bt_device_idx = -1;
static int selected_song_idx      = -1;
static bool song_paused;
static esp_timer_handle_t bt_scan_refresh_timer;
static screen_state current_screen = SCREEN_STATE_BT_DISCOVERY;

static void add_bt_device(const char* name, esp_bd_addr_t bda);
static int find_bt_device(esp_bd_addr_t bda);
static void screen_refresh_bt_scan(void* arg);
static void screen_show_bt_scan(void);
static esp_err_t screen_add_bt_device(const char* name, esp_bd_addr_t bda);
static esp_err_t screen_handle_btn_press(screen_button button);
static esp_err_t screen_handle_bt_discovery_btn_press(screen_button button);
static esp_err_t screen_handle_song_select_btn_press(screen_button button);
static esp_err_t screen_handle_song_playing_btn_press(screen_button button);
static const char* screen_button_to_str(screen_button button);
static int find_first_bt_device(void);
static int find_next_bt_device(int start_idx);
static int find_prev_bt_device(int start_idx);
static void screen_show_song_selection(void);
static void screen_show_song_loading(void);
static void screen_show_song_playing(void);
static void screen_update_song_progress(uint32_t elapsed_seconds, uint32_t total_seconds);

static QueueHandle_t screen_event_queue = NULL;
static TaskHandle_t screen_task_handle  = NULL;

static void screen_task_handler(void* arg __attribute__((unused))) {
    screen_msg msg;
    while (1) {
        if (pdTRUE == xQueueReceive(screen_event_queue, &msg, (TickType_t)portMAX_DELAY)) {
            ESP_LOGD(TAG, "%s, event: 0x%x", __func__, msg.event);

            switch (msg.event) {
            case SCREEN_EVT_REFRESH_BT_SCAN:
                screen_refresh_bt_scan(NULL);
                break;

            case SCREEN_EVT_BTN_PRESS:
                ESP_LOGI(TAG, "Button press: %s (%d)", screen_button_to_str(msg.button), msg.button);
                if (current_screen == msg.screen_state) {
                    screen_handle_btn_press(msg.button);
                }
                break;

            case SCREEN_EVT_BT_DEVICE_FOUND:
                screen_add_bt_device(msg.device_name, msg.bda);
                break;

            case SCREEN_EVT_BT_DEVICE_CONNECTED:
                current_screen = SCREEN_STATE_SONG_SELECT;
                song_paused = false;
                screen_show_song_selection();
                break;

            case SCREEN_EVT_SONG_PLAYING:
                current_screen = SCREEN_STATE_SONG_PLAYING;
                song_paused = false;
                screen_show_song_playing();
                break;

            case SCREEN_EVT_SONG_PROGRESS:
                screen_update_song_progress(msg.elapsed_seconds, msg.total_seconds);
                break;

            default:
                ESP_LOGW(TAG, "%s, unhandled event: %d", __func__, msg.event);
                break;
            }
        }
    }
}

esp_err_t screen_init(void) {
    ESP_RETURN_ON_ERROR(screen_render_init(), TAG, "Screen render init failed");

    screen_event_queue = xQueueCreate(10, sizeof(screen_msg));
    ESP_RETURN_ON_FALSE(screen_event_queue != NULL, ESP_ERR_NO_MEM, TAG, "Screen queue create failed");

    BaseType_t task_created = xTaskCreate(screen_task_handler, "ScreenTask", 3072, NULL, 10, &screen_task_handle);
    ESP_RETURN_ON_FALSE(task_created == pdPASS, ESP_ERR_NO_MEM, TAG, "Screen task create failed");

    const esp_timer_create_args_t timer_args = {
        .callback = screen_notify_bt_refresh,
        .name     = "bt_scan_refresh",
    };

    ESP_RETURN_ON_ERROR(
        esp_timer_create(&timer_args, &bt_scan_refresh_timer), TAG, "BT scan refresh timer create failed");

    ESP_RETURN_ON_ERROR(
        esp_timer_start_periodic(bt_scan_refresh_timer, 5 * 1000 * 1000), TAG, "BT scan refresh timer start failed");

    return ESP_OK;
}

static const char* screen_button_to_str(screen_button button) {
    static const char* const button_names[] = {
        [SCREEN_BTN_UP]     = "SCREEN_BTN_UP",
        [SCREEN_BTN_DOWN]   = "SCREEN_BTN_DOWN",
        [SCREEN_BTN_LEFT]   = "SCREEN_BTN_LEFT",
        [SCREEN_BTN_RIGHT]  = "SCREEN_BTN_RIGHT",
        [SCREEN_BTN_SELECT] = "SCREEN_BTN_SELECT",
    };

    if (button >= SCREEN_BTN_MAX || button_names[button] == NULL) {
        return "UNKNOWN";
    }

    return button_names[button];
}

void screen_notify_bt_device_found(const char* device_name, esp_bd_addr_t bda) {
    screen_msg msg = {.event = SCREEN_EVT_BT_DEVICE_FOUND};
    strncpy(msg.device_name, device_name, MAX_DEVICE_NAME_LEN);
    memcpy(msg.bda, bda, sizeof(esp_bd_addr_t));
    xQueueSend(screen_event_queue, &msg, 0);
}

void screen_notify_bt_refresh(void* arg) {
    (void)arg;
    screen_msg msg = {.event = SCREEN_EVT_REFRESH_BT_SCAN, .device_name = "\0"};
    xQueueSend(screen_event_queue, &msg, 0);
}

void screen_notify_button_press(screen_button button) {
    screen_msg msg = {.event = SCREEN_EVT_BTN_PRESS, .button = button, .screen_state = current_screen};
    xQueueSend(screen_event_queue, &msg, 0);
}

void screen_notify_show_song_selection(void) {
    screen_msg msg = {.event = SCREEN_EVT_BT_DEVICE_CONNECTED};
    xQueueSend(screen_event_queue, &msg, 0);
}

void screen_notify_show_song_playing(void) {
    screen_msg msg = {.event = SCREEN_EVT_SONG_PLAYING};
    xQueueSend(screen_event_queue, &msg, 0);
}

void screen_notify_song_progress(uint32_t elapsed_seconds, uint32_t total_seconds) {
    screen_msg msg = {
        .event = SCREEN_EVT_SONG_PROGRESS,
        .elapsed_seconds = elapsed_seconds,
        .total_seconds = total_seconds,
    };
    xQueueSend(screen_event_queue, &msg, 0);
}

static void screen_remove_old_bt_devices(void) {
    int64_t now = esp_timer_get_time() / 1000;
    for (int i = 0; i < MAX_BT_DEVICE_NUM; i++) {
        if (bt_devices[i].name[0] == '\0') {
            continue;
        }
        if (bt_devices[i].last_discovered_ms < (now - MAX_BT_DEVICE_TIMEOUT_MS)) {
            memset(&bt_devices[i], 0, sizeof(screen_bt_device_t));
            num_bt_devices--;
            if (selected_bt_device_idx == i) {
                selected_bt_device_idx = find_first_bt_device();
            }
        }
    }
}

static void add_bt_device(const char* name, esp_bd_addr_t bda) {
    for (int i = 0; i < MAX_BT_DEVICE_NUM; i++) {
        if (bt_devices[i].name[0] == '\0') {
            strncpy(bt_devices[i].name, name, MAX_DEVICE_NAME_LEN);
            memcpy(bt_devices[i].bda, bda, sizeof(bt_devices[i].bda));
            bt_devices[i].last_discovered_ms = esp_timer_get_time() / 1000;
            num_bt_devices++;
            if (selected_bt_device_idx < 0) {
                selected_bt_device_idx = i;
            }
            break;
        }
    }
}

static int find_bt_device(esp_bd_addr_t bda) {
    for (int i = 0; i < MAX_BT_DEVICE_NUM; i++) {
        if (bt_devices[i].name[0] == '\0') {
            continue;
        }
        if (memcmp(bt_devices[i].bda, bda, ESP_BD_ADDR_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_first_bt_device(void) {
    for (int i = 0; i < MAX_BT_DEVICE_NUM; i++) {
        if (bt_devices[i].name[0] != '\0') {
            return i;
        }
    }

    return -1;
}

static int find_next_bt_device(int start_idx) {
    if (num_bt_devices == 0) {
        return -1;
    }

    for (int offset = 1; offset <= MAX_BT_DEVICE_NUM; offset++) {
        int idx = (start_idx + offset) % MAX_BT_DEVICE_NUM;
        if (bt_devices[idx].name[0] != '\0') {
            return idx;
        }
    }

    return -1;
}

static int find_prev_bt_device(int start_idx) {
    if (num_bt_devices == 0) {
        return -1;
    }

    for (int offset = 1; offset <= MAX_BT_DEVICE_NUM; offset++) {
        int idx = (start_idx - offset + MAX_BT_DEVICE_NUM) % MAX_BT_DEVICE_NUM;
        if (bt_devices[idx].name[0] != '\0') {
            return idx;
        }
    }

    return -1;
}

static void screen_refresh_bt_scan(void* arg) {
    (void)arg;
    screen_remove_old_bt_devices();
    screen_show_bt_scan();
}

static esp_err_t screen_add_bt_device(const char* name, esp_bd_addr_t bda) {
    screen_remove_old_bt_devices();
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (name[0] == '\0' || strnlen(name, MAX_DEVICE_NAME_LEN) >= MAX_DEVICE_NAME_LEN) {
        ESP_LOGE(TAG, "Device name too long");
        return ESP_ERR_INVALID_SIZE;
    }
    int idx = find_bt_device(bda);
    if (idx >= 0) {
        bt_devices[idx].last_discovered_ms = esp_timer_get_time() / 1000;
    } else if (num_bt_devices < MAX_BT_DEVICE_NUM) {
        add_bt_device(name, bda);
    }

    return ESP_OK;
}

static esp_err_t screen_handle_btn_press(screen_button button) {
    switch (current_screen) {
    case SCREEN_STATE_BT_DISCOVERY:
        return screen_handle_bt_discovery_btn_press(button);
    case SCREEN_STATE_SONG_SELECT:
        return screen_handle_song_select_btn_press(button);
    case SCREEN_STATE_SONG_PLAYING:
        return screen_handle_song_playing_btn_press(button);
    case SCREEN_STATE_SONG_LOADING:
    default:
        return ESP_OK;
    }
}

static esp_err_t screen_handle_bt_discovery_btn_press(screen_button button) {
    if (selected_bt_device_idx < 0 || bt_devices[selected_bt_device_idx].name[0] == '\0') {
        selected_bt_device_idx = find_first_bt_device();
    }

    switch (button) {
    case SCREEN_BTN_UP:
        selected_bt_device_idx = find_prev_bt_device(selected_bt_device_idx);
        screen_show_bt_scan();
        break;
    case SCREEN_BTN_DOWN:
        selected_bt_device_idx = find_next_bt_device(selected_bt_device_idx);
        screen_show_bt_scan();
        break;
    case SCREEN_BTN_SELECT:
        if (selected_bt_device_idx >= 0) {
            bt_app_connect_to(bt_devices[selected_bt_device_idx].name, bt_devices[selected_bt_device_idx].bda);
        }
        break;
    default:
        break;
    }

    return ESP_OK;
}

static esp_err_t screen_handle_song_select_btn_press(screen_button button) {
    size_t songs_count = sdcard_get_song_count();

    if (songs_count == 0) {
        selected_song_idx = -1;
        return ESP_OK;
    }

    if (selected_song_idx < 0 || selected_song_idx >= (int)songs_count) {
        selected_song_idx = 0;
    }

    switch (button) {
    case SCREEN_BTN_UP:
        selected_song_idx = (selected_song_idx - 1 + (int)songs_count) % (int)songs_count;
        screen_show_song_selection();
        break;
    case SCREEN_BTN_DOWN:
        selected_song_idx = (selected_song_idx + 1) % (int)songs_count;
        screen_show_song_selection();
        break;
    case SCREEN_BTN_SELECT:
        current_screen = SCREEN_STATE_SONG_LOADING;
        song_paused = false;
        screen_show_song_loading();
        ESP_RETURN_ON_ERROR(player_play(selected_song_idx), TAG, "Failed to start player");
        break;
    default:
        break;
    }

    return ESP_OK;
}

static esp_err_t screen_handle_song_playing_btn_press(screen_button button) {
    esp_err_t ret;

    if (button != SCREEN_BTN_SELECT) {
        return ESP_OK;
    }

    ret = song_paused ? player_resume() : player_pause();
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to toggle playback pause");

    song_paused = !song_paused;
    screen_render_song_paused(song_paused);
    return ESP_OK;
}

static void screen_show_bt_scan(void) {
    if (current_screen != SCREEN_STATE_BT_DISCOVERY) {
        return;
    }

    screen_render_bt_scan(bt_devices, MAX_BT_DEVICE_NUM, num_bt_devices, selected_bt_device_idx);
}

static void screen_show_song_selection(void) {
    size_t songs_count = sdcard_get_song_count();

    if (songs_count == 0) {
        selected_song_idx = -1;
    } else if (selected_song_idx < 0 || selected_song_idx >= (int)songs_count) {
        selected_song_idx = 0;
    }

    screen_render_song_selection(selected_song_idx);
}

static void screen_show_song_loading(void) {
    screen_render_song_loading(selected_song_idx);
}

static void screen_show_song_playing(void) {
    screen_render_song_playing(selected_song_idx);
}

static void screen_update_song_progress(uint32_t elapsed_seconds, uint32_t total_seconds) {
    if (current_screen != SCREEN_STATE_SONG_PLAYING) {
        return;
    }

    screen_render_song_progress(elapsed_seconds, total_seconds);
}
