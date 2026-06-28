#include "buttons.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "screen.h"

#define TAG "Buttons"

#define BUTTON_SELECT GPIO_NUM_4
#define BUTTON_UP GPIO_NUM_21
#define BUTTON_DOWN GPIO_NUM_32

#define BUTTON_DEBOUNCE_MS 200

static QueueHandle_t buttons_event_queue = NULL;
static TaskHandle_t buttons_task_handle  = NULL;
static TickType_t last_button_ticks[SCREEN_BTN_MAX];

typedef struct {
    screen_button event;
} button_msg;

static void IRAM_ATTR button_select_isr_handler(void* arg) {
    button_msg msg = {
        .event = (screen_button)(uintptr_t)arg,
    };
    BaseType_t higher_priority_task_woken = pdFALSE;

    if (buttons_event_queue != NULL) {
        xQueueSendFromISR(buttons_event_queue, &msg, &higher_priority_task_woken);
    }

    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void buttons_task_handler(void* arg __attribute__((unused))) {
    button_msg msg;

    while (1) {
        if (pdTRUE == xQueueReceive(buttons_event_queue, &msg, (TickType_t)portMAX_DELAY)) {
            ESP_LOGI(TAG, "%s, event: 0x%x", __func__, msg.event);

            switch (msg.event) {
            case SCREEN_BTN_UP:
            case SCREEN_BTN_DOWN:
            case SCREEN_BTN_LEFT:
            case SCREEN_BTN_RIGHT:
            case SCREEN_BTN_SELECT: {
                TickType_t now = xTaskGetTickCount();
                if ((now - last_button_ticks[msg.event]) >= pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS)) {
                    last_button_ticks[msg.event] = now;
                    screen_notify_button_press(msg.event);
                }
                break;
            }
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

    const gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << BUTTON_SELECT) | (1ULL << BUTTON_UP) | (1ULL << BUTTON_DOWN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&button_config), TAG, "Select button GPIO config failed");

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

    BaseType_t task_created = xTaskCreate(buttons_task_handler, "ButtonsTask", 3072, NULL, 10, &buttons_task_handle);
    ESP_RETURN_ON_FALSE(task_created == pdPASS, ESP_ERR_NO_MEM, TAG, "Button task create failed");

    return ESP_OK;
}
