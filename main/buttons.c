#include "buttons.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "screen.h"

#define TAG "Buttons"

#define BUTTON_SELECT GPIO_NUM_4
#define BUTTON_UP GPIO_NUM_21
#define BUTTON_DOWN GPIO_NUM_32
#define BUTTON_RIGHT GPIO_NUM_34
#define BUTTON_LEFT GPIO_NUM_35

#define BUTTON_DEBOUNCE_MS 200
#define BUTTON_SELECT_DEBOUNCE_MS 30
#define BUTTON_LONG_PRESS_MS 700
#define BUTTON_TASK_PRIORITY 10

static QueueHandle_t buttons_event_queue     = NULL;
static TaskHandle_t buttons_task_handle      = NULL;
static TimerHandle_t select_debounce_timer   = NULL;
static TimerHandle_t select_long_press_timer = NULL;
static TickType_t last_button_isr_ticks[SCREEN_BTN_MAX];
static bool was_pressed;
static bool select_long_press_sent;

typedef struct {
    screen_button event;
} button_msg;

static void enqueue_button_event(screen_button event) {
    button_msg msg = {
        .event = event,
    };

    if (buttons_event_queue == NULL || xQueueSend(buttons_event_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to enqueue button event: %d", event);
    }
}

static void select_debounce_timer_cb(TimerHandle_t timer) {
    (void)timer;
    bool is_pressed_now = gpio_get_level(BUTTON_SELECT) == 0;

    if (is_pressed_now == was_pressed) {
        return;
    }

    was_pressed = is_pressed_now;
    if (is_pressed_now) {
        select_long_press_sent = false;
        if (xTimerReset(select_long_press_timer, 0) != pdPASS) {
            ESP_LOGW(TAG, "Failed to start Select long-press timer");
        }
        return;
    }

    xTimerStop(select_long_press_timer, 0);
    if (!select_long_press_sent) {
        enqueue_button_event(SCREEN_BTN_SELECT);
    }
}

static void select_long_press_timer_cb(TimerHandle_t timer) {
    (void)timer;

    if (!was_pressed || gpio_get_level(BUTTON_SELECT) != 0) {
        return;
    }

    select_long_press_sent = true;
    enqueue_button_event(SCREEN_BTN_SELECT_LONG);
}

static void IRAM_ATTR button_select_isr_handler(void* arg) {
    screen_button event = (screen_button)(uintptr_t)arg;
    BaseType_t higher_priority_task_woken = pdFALSE;

    if (event < 0 || event >= SCREEN_BTN_SELECT_LONG) {
        return;
    }

    if (event == SCREEN_BTN_SELECT) {
        xTimerResetFromISR(select_debounce_timer, &higher_priority_task_woken);
        if (higher_priority_task_woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
        return;
    }

    TickType_t now = xTaskGetTickCountFromISR();
    if (last_button_isr_ticks[event] != 0 && (now - last_button_isr_ticks[event]) < pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS)) {
        return;
    }

    button_msg msg = {
        .event = event,
    };

    if (buttons_event_queue != NULL) {
        if (xQueueSendFromISR(buttons_event_queue, &msg, &higher_priority_task_woken) == pdTRUE) {
            last_button_isr_ticks[event] = now;
        }
    }

    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void buttons_task_handler(void* arg __attribute__((unused))) {
    button_msg msg;

    while (1) {
        if (pdTRUE == xQueueReceive(buttons_event_queue, &msg, (TickType_t)portMAX_DELAY)) {
            switch (msg.event) {
            case SCREEN_BTN_UP:
            case SCREEN_BTN_DOWN:
            case SCREEN_BTN_LEFT:
            case SCREEN_BTN_RIGHT:
            case SCREEN_BTN_SELECT:
            case SCREEN_BTN_SELECT_LONG:
                ESP_LOGI(TAG, "%s, event: 0x%x", __func__, msg.event);
                screen_notify_button_press(msg.event);
                break;
            default:
                ESP_LOGW(TAG, "%s, unhandled event: %d", __func__, msg.event);
                break;
            }
        }
    }
}

esp_err_t buttons_init(void) {
    buttons_event_queue = xQueueCreate(10, sizeof(button_msg));
    ESP_RETURN_ON_FALSE(buttons_event_queue != NULL, ESP_ERR_NO_MEM, TAG, "Button queue create failed");

    select_debounce_timer =
        xTimerCreate("SelectDebounce", pdMS_TO_TICKS(BUTTON_SELECT_DEBOUNCE_MS), pdFALSE, NULL, select_debounce_timer_cb);
    ESP_RETURN_ON_FALSE(select_debounce_timer != NULL, ESP_ERR_NO_MEM, TAG, "Select debounce timer create failed");

    select_long_press_timer =
        xTimerCreate("SelectLongPress", pdMS_TO_TICKS(BUTTON_LONG_PRESS_MS), pdFALSE, NULL, select_long_press_timer_cb);
    ESP_RETURN_ON_FALSE(select_long_press_timer != NULL, ESP_ERR_NO_MEM, TAG, "Select long press timer create failed");

    const gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << BUTTON_SELECT) | (1ULL << BUTTON_UP) | (1ULL << BUTTON_DOWN) | (1ULL << BUTTON_RIGHT) |
                        (1ULL << BUTTON_LEFT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&button_config), TAG, "Button GPIO config failed");
    ESP_RETURN_ON_ERROR(gpio_set_intr_type(BUTTON_SELECT, GPIO_INTR_ANYEDGE), TAG, "Select button interrupt config failed");

    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(ret, TAG, "GPIO ISR service install failed");
    }

    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(BUTTON_SELECT, button_select_isr_handler, (void*)SCREEN_BTN_SELECT),
                        TAG,
                        "Select button ISR add failed");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(BUTTON_UP, button_select_isr_handler, (void*)SCREEN_BTN_UP),
                        TAG,
                        "Up button ISR add failed");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(BUTTON_DOWN, button_select_isr_handler, (void*)SCREEN_BTN_DOWN),
                        TAG,
                        "Down button ISR add failed");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(BUTTON_RIGHT, button_select_isr_handler, (void*)SCREEN_BTN_RIGHT),
                        TAG,
                        "Right button ISR add failed");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(BUTTON_LEFT, button_select_isr_handler, (void*)SCREEN_BTN_LEFT),
                        TAG,
                        "Left button ISR add failed");

    BaseType_t task_created =
        xTaskCreate(buttons_task_handler, "ButtonsTask", 3072, NULL, BUTTON_TASK_PRIORITY, &buttons_task_handle);
    ESP_RETURN_ON_FALSE(task_created == pdPASS, ESP_ERR_NO_MEM, TAG, "Button task create failed");

    return ESP_OK;
}
