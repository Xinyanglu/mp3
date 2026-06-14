idf_component_register(
    SRCS "main.c"
         "screen.c"
         "buttons.c"
         "bt/bt_app.c"
         "bt/bt_app_core.c"
         "bt/bt_a2dp.c"
         "bt/bt_avrcp.c"
         "bt/bt_gap.c"
         "bt/bt_state.c"
         "bt/bt_utils.c"
    INCLUDE_DIRS "." "bt"
    PRIV_REQUIRES "bt" "esp_driver_gpio" "nvs_flash" "esp_lcd" "esp_lvgl_port"
)
