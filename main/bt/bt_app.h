/*
 * Public Bluetooth app entry point.
 *
 * Exposes the single initializer used by app_main() to bring up the Bluetooth
 * controller, Bluedroid host stack, A2DP source, AVRCP, GAP discovery, and the
 * app work queue.
 */
#pragma once

void init_bt_app(void);
