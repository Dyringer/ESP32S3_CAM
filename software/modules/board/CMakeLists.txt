idf_component_register(
    INCLUDE_DIRS "${BOARD_DIR}"
    REQUIRES
        esp_hal_gpio
        esp_hal_i2c
        esp_hal_ledc
)
