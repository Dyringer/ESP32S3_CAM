idf_component_register(
    SRCS
        "camera.c"
        "camera_shell.c"
    INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        esp32-camera     # esp_camera_*, sensor_t (managed component espressif/esp32-camera)
        board            # BSP_CAMERA_* / BSP_LED_* pins
        common           # ARRAY_SIZE
        esp_driver_ledc  # LED PWM
        heap             # heap_caps_get_total_size (PSRAM check)
        freertos
        log
        nvs_flash        # saved settings (nvs.h)
        shell            # app_shell_register_cmd
)
