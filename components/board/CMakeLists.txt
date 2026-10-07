idf_component_register(
    INCLUDE_DIRS "${BOARD_DIR}"
    REQUIRES
        esp_hal_gpio    # soc/gpio_num.h     — GPIO_NUM_x in bsp.h
        esp_hal_ledc    # hal/ledc_types.h   — LEDC_TIMER_x / LEDC_CHANNEL_x in bsp.h
)
