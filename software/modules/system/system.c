/**
 ******************************************************************************
 * @file    system.c
 * @author  Daniel Dyringer
 * @date    2026-10-07
 * @brief   Device-wide actions — reboot, factory reset.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "system.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SYSTEM_RESTART_DELAY_MS     300u

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* system_tag = "system";

static portMUX_TYPE system_reboot_lock = portMUX_INITIALIZER_UNLOCKED;
static bool system_reboot_scheduled    = false;


/* Private functions ------------------------------------------------------- */

static void system_restart_cb(void* arg)
{
    ARG_UNUSED(arg);

    esp_restart();
}


/* Exposed API ------------------------------------------------------------- */

/* Returns at once: callers include httpd handlers, which must not block. The
 * delay lets the last shell/log output and HTTP replies reach the client. */
void system_reboot(void)
{
    bool already_scheduled = false;

    taskENTER_CRITICAL(&system_reboot_lock);
    already_scheduled       = system_reboot_scheduled;
    system_reboot_scheduled = true;
    taskEXIT_CRITICAL(&system_reboot_lock);

    if (!already_scheduled)
    {
        ESP_LOGW(system_tag, "Rebooting in %u ms", SYSTEM_RESTART_DELAY_MS);

        const esp_timer_create_args_t timer_config = {
            .callback = system_restart_cb,
            .name     = "system_restart",
        };
        esp_timer_handle_t timer = NULL;
        esp_err_t error_code     = esp_timer_create(&timer_config, &timer);

        if (error_code == ESP_OK)
        {
            error_code =
                esp_timer_start_once(timer, (uint64_t)SYSTEM_RESTART_DELAY_MS * 1000u);
        }

        if (error_code != ESP_OK)
        {
            ESP_LOGE(
                system_tag,
                "Restart timer failed: %s, rebooting now",
                esp_err_to_name(error_code));
            esp_restart();
        }
    }
}

esp_err_t system_factory_reset(void)
{
    ESP_LOGW(system_tag, "Factory reset — erasing all settings");

    const esp_err_t error_code = nvs_flash_erase();

    if (error_code != ESP_OK)
    {
        ESP_LOGE(
            system_tag, "NVS erase failed: %s — not rebooting", esp_err_to_name(error_code));
    }
    else
    {
        ESP_LOGW(system_tag, "Settings erased, rebooting into the setup portal");
        system_reboot();
    }

    return error_code;
}

/**** END OF FILE ****/
