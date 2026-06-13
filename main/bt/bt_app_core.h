/*
 * Bluetooth app work-queue interface.
 *
 * Declares the dispatch API used by Bluetooth callbacks to hand work to the app
 * task, keeping profile logic serialized and outside callback context.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BT_APP_CORE_TAG "BT_APP_CORE"

#define BT_APP_SIG_WORK_DISPATCH 0x01

typedef void (*bt_app_cb_t)(uint16_t event, void *param);

typedef struct {
    uint16_t sig;
    uint16_t event;
    bt_app_cb_t cb;
    void *param;
} bt_app_msg_t;

typedef void (*bt_app_copy_cb_t)(void *p_dest, void *p_src, int len);

bool bt_app_work_dispatch(bt_app_cb_t p_cback, uint16_t event, void *p_params, int param_len,
                          bt_app_copy_cb_t p_copy_cback);
void bt_app_task_start_up(void);
void bt_app_task_shut_down(void);
