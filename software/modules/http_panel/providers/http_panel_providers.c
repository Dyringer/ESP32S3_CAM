/**
 ******************************************************************************
 * @file    http_panel_providers.c
 * @author  Daniel Dyringer
 * @date    2026-03-09
 * @brief   HTTP panel providers — formatted device values and actions
 *          shared by the panel pages.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "http_panel_providers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp.h"
#include "drivers/ltr308_driver.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_clk_tree.h"
#include "esp_flash.h"
#include "esp_flash_chips/esp_flash_types.h"
#include "esp_idf_version.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota.h"
#include "sensor_store.h"
#include "sysmon.h"
#include "system.h"
#include "wifi_connection.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define HTTP_PANEL_PROVIDERS_EM_DASH    "\xe2\x80\x94"

/* debug.conf / release.conf differ only in the optimisation level. */
#if CONFIG_COMPILER_OPTIMIZATION_DEBUG
#define HTTP_PANEL_PROVIDERS_BUILD_TYPE "debug, -Og"
#elif CONFIG_COMPILER_OPTIMIZATION_NONE
#define HTTP_PANEL_PROVIDERS_BUILD_TYPE "debug, -O0"
#elif CONFIG_COMPILER_OPTIMIZATION_PERF
#define HTTP_PANEL_PROVIDERS_BUILD_TYPE "release, -O2"
#elif CONFIG_COMPILER_OPTIMIZATION_SIZE
#define HTTP_PANEL_PROVIDERS_BUILD_TYPE "release, -Os"
#else
#define HTTP_PANEL_PROVIDERS_BUILD_TYPE "unknown"
#endif

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

/* Read once: every flash command stalls both cores' flash cache. */
static uint32_t http_panel_providers_flash_id = 0u;


/* Private functions ------------------------------------------------------- */

static const char* http_panel_providers_chip_model_name(esp_chip_model_t model)
{
    const char* name = NULL;

    switch (model)
    {
        case CHIP_ESP32:
        {
            name = "ESP32";
        }
        break;
        case CHIP_ESP32S2:
        {
            name = "ESP32-S2";
        }
        break;
        case CHIP_ESP32S3:
        {
            name = "ESP32-S3";
        }
        break;
        case CHIP_ESP32C3:
        {
            name = "ESP32-C3";
        }
        break;
        default:
        {
            name = "unknown";
        }
        break;
    }

    return name;
}

/* JEDEC manufacturer IDs of flash vendors found on Espressif modules. */
static const char* http_panel_providers_flash_vendor_name(uint32_t manufacturer)
{
    const char* name = NULL;

    switch (manufacturer)
    {
        case 0x0Bu:
        {
            name = "XTX";
        }
        break;
        case 0x20u:
        {
            name = "XMC";
        }
        break;
        case 0x5Eu:
        {
            name = "Zbit";
        }
        break;
        case 0x68u:
        {
            name = "Boya";
        }
        break;
        case 0x85u:
        {
            name = "Puya";
        }
        break;
        case 0x9Du:
        {
            name = "ISSI";
        }
        break;
        case 0xC2u:
        {
            name = "Macronix";
        }
        break;
        case 0xC8u:
        {
            name = "GigaDevice";
        }
        break;
        case 0xEFu:
        {
            name = "Winbond";
        }
        break;
        default:
        {
            name = "unknown vendor";
        }
        break;
    }

    return name;
}

static const char* http_panel_providers_flash_mode_name(esp_flash_io_mode_t mode)
{
    const char* name = NULL;

    switch (mode)
    {
        case SPI_FLASH_SLOWRD:
        {
            name = "Slow read";
        }
        break;
        case SPI_FLASH_FASTRD:
        {
            name = "Fast read";
        }
        break;
        case SPI_FLASH_DOUT:
        {
            name = "DOUT";
        }
        break;
        case SPI_FLASH_DIO:
        {
            name = "DIO";
        }
        break;
        case SPI_FLASH_QOUT:
        {
            name = "QOUT";
        }
        break;
        case SPI_FLASH_QIO:
        {
            name = "QIO";
        }
        break;
        case SPI_FLASH_OPI_STR:
        {
            name = "OPI STR";
        }
        break;
        case SPI_FLASH_OPI_DTR:
        {
            name = "OPI DTR";
        }
        break;
        default:
        {
            name = "unknown";
        }
        break;
    }

    return name;
}

static const char* http_panel_providers_ota_state_name(esp_ota_img_states_t state)
{
    const char* name = NULL;

    switch (state)
    {
        case ESP_OTA_IMG_NEW:
        {
            name = "new (first boot)";
        }
        break;
        case ESP_OTA_IMG_PENDING_VERIFY:
        {
            name = "pending verify (rollback if not confirmed)";
        }
        break;
        case ESP_OTA_IMG_VALID:
        {
            name = "valid";
        }
        break;
        case ESP_OTA_IMG_INVALID:
        {
            name = "invalid";
        }
        break;
        case ESP_OTA_IMG_ABORTED:
        {
            name = "aborted";
        }
        break;
        default:
        {
            name = "undefined";
        }
        break;
    }

    return name;
}

static const char* http_panel_providers_reset_reason_name(esp_reset_reason_t reason)
{
    const char* name = NULL;

    switch (reason)
    {
        case ESP_RST_POWERON:
        {
            name = "power-on";
        }
        break;
        case ESP_RST_EXT:
        {
            name = "external pin";
        }
        break;
        case ESP_RST_SW:
        {
            name = "software";
        }
        break;
        case ESP_RST_PANIC:
        {
            name = "panic";
        }
        break;
        case ESP_RST_INT_WDT:
        {
            name = "interrupt watchdog";
        }
        break;
        case ESP_RST_TASK_WDT:
        {
            name = "task watchdog";
        }
        break;
        case ESP_RST_WDT:
        {
            name = "other watchdog";
        }
        break;
        case ESP_RST_DEEPSLEEP:
        {
            name = "deep sleep";
        }
        break;
        case ESP_RST_BROWNOUT:
        {
            name = "brownout";
        }
        break;
        case ESP_RST_SDIO:
        {
            name = "SDIO";
        }
        break;
        case ESP_RST_USB:
        {
            name = "USB";
        }
        break;
        case ESP_RST_JTAG:
        {
            name = "JTAG";
        }
        break;
        default:
        {
            name = "unknown";
        }
        break;
    }

    return name;
}

static const char* http_panel_providers_wifi_mode_name(wifi_mode_t mode)
{
    const char* name = NULL;

    switch (mode)
    {
        case WIFI_MODE_STA:
        {
            name = "STA";
        }
        break;
        case WIFI_MODE_AP:
        {
            name = "AP";
        }
        break;
        case WIFI_MODE_APSTA:
        {
            name = "AP-STA";
        }
        break;
        default:
        {
            name = "OFF";
        }
        break;
    }

    return name;
}


/* Exposed API ------------------------------------------------------------- */

/* ---- Runtime status ----------------------------------------------------- */

void hp_get_uptime(char* buffer, size_t length)
{
    const int64_t uptime_us      = esp_timer_get_time();
    const uint32_t total_seconds = (uint32_t)(uptime_us / 1000000LL);
    const uint32_t hours         = total_seconds / 3600u;
    const uint32_t minutes       = (total_seconds % 3600u) / 60u;
    const uint32_t seconds       = total_seconds % 60u;

    snprintf(
        buffer,
        length,
        "%luh %02lum %02lus",
        (unsigned long)hours,
        (unsigned long)minutes,
        (unsigned long)seconds);
}

void hp_add_cpu_load(cJSON* object)
{
    cJSON* const cpu = cJSON_AddArrayToObject(object, "cpu");

    for (int core = 0; core < SYSMON_CORE_COUNT; core++)
    {
        cJSON_AddItemToArray(cpu, cJSON_CreateNumber(sysmon_cpu_load(core)));
    }
}

void hp_get_wifi_state(char* buffer, size_t length)
{
    wifi_mode_t mode = WIFI_MODE_NULL;

    if (esp_wifi_get_mode(&mode) != ESP_OK)
    {
        mode = WIFI_MODE_NULL;
    }

    snprintf(
        buffer,
        length,
        "%s %s",
        http_panel_providers_wifi_mode_name(mode),
        wifi_connection_state_str(wifi_connection_get_state()));
}

void hp_get_task_count(char* buffer, size_t length)
{
    snprintf(buffer, length, "%u", (unsigned)uxTaskGetNumberOfTasks());
}

void hp_get_reset_reason(char* buffer, size_t length)
{
    strlcpy(buffer, http_panel_providers_reset_reason_name(esp_reset_reason()), length);
}

/* ---- Device configuration ----------------------------------------------- */

void hp_get_board(char* buffer, size_t length)
{
    strlcpy(buffer, BOARD_NAME, length);
}

void hp_get_chip(char* buffer, size_t length)
{
    esp_chip_info_t info = {0};
    esp_chip_info(&info);

    strlcpy(buffer, http_panel_providers_chip_model_name(info.model), length);
}

void hp_get_chip_revision(char* buffer, size_t length)
{
    esp_chip_info_t info = {0};
    esp_chip_info(&info);

    /* revision is encoded as major * 100 + minor */
    snprintf(
        buffer,
        length,
        "v%u.%u",
        (unsigned)(info.revision / 100u),
        (unsigned)(info.revision % 100u));
}

void hp_get_core_count(char* buffer, size_t length)
{
    esp_chip_info_t info = {0};
    esp_chip_info(&info);

    snprintf(buffer, length, "%u", (unsigned)info.cores);
}

void hp_get_tick_rate(char* buffer, size_t length)
{
    snprintf(buffer, length, "%u Hz", (unsigned)configTICK_RATE_HZ);
}

void hp_get_cpu_freq(char* buffer, size_t length)
{
    uint32_t frequency_hz = 0u;

    if (esp_clk_tree_src_get_freq_hz(
            SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &frequency_hz)
        == ESP_OK)
    {
        snprintf(buffer, length, "%lu MHz", (unsigned long)(frequency_hz / 1000000u));
    }
    else
    {
        snprintf(buffer, length, HTTP_PANEL_PROVIDERS_EM_DASH);
    }
}

void hp_get_flash_size(char* buffer, size_t length)
{
    uint32_t size = 0u;

    if (esp_flash_get_size(NULL, &size) == ESP_OK)
    {
        snprintf(buffer, length, "%lu MB", (unsigned long)(size / (1024u * 1024u)));
    }
    else
    {
        snprintf(buffer, length, HTTP_PANEL_PROVIDERS_EM_DASH);
    }
}

void hp_get_idf_version(char* buffer, size_t length)
{
    strlcpy(buffer, esp_get_idf_version(), length);
}

void hp_get_build_date(char* buffer, size_t length)
{
    const esp_app_desc_t* const app_description = esp_app_get_description();
    snprintf(
        buffer,
        length,
        "%s %s (" HTTP_PANEL_PROVIDERS_BUILD_TYPE ")",
        app_description->date,
        app_description->time);
}

void hp_get_partition(char* buffer, size_t length)
{
    const esp_partition_t* const running = esp_ota_get_running_partition();
    strlcpy(buffer, (running != NULL) ? running->label : "?", length);
}

/* ---- Memory chips ------------------------------------------------------- */

void hp_get_flash_id(char* buffer, size_t length)
{
    if (http_panel_providers_flash_id == 0u)
    {
        uint32_t id = 0u;

        if (esp_flash_read_id(NULL, &id) == ESP_OK)
        {
            http_panel_providers_flash_id = id;
        }
    }

    if (http_panel_providers_flash_id == 0u)
    {
        snprintf(buffer, length, HTTP_PANEL_PROVIDERS_EM_DASH);
    }
    else
    {
        snprintf(
            buffer,
            length,
            "%s (0x%06lX)",
            http_panel_providers_flash_vendor_name(http_panel_providers_flash_id >> 16u),
            (unsigned long)http_panel_providers_flash_id);
    }
}

void hp_get_flash_mode(char* buffer, size_t length)
{
    strlcpy(
        buffer,
        http_panel_providers_flash_mode_name(esp_flash_default_chip->read_mode),
        length);
}

void hp_get_flash_speed(char* buffer, size_t length)
{
    snprintf(
        buffer,
        length,
        "%lu MHz",
        (unsigned long)strtoul(CONFIG_ESPTOOLPY_FLASHFREQ, NULL, 10));
}

void hp_get_app_partition(char* buffer, size_t length)
{
    const esp_partition_t* const running = esp_ota_get_running_partition();

    if (running == NULL)
    {
        snprintf(buffer, length, HTTP_PANEL_PROVIDERS_EM_DASH);
    }
    else
    {
        snprintf(
            buffer,
            length,
            "%s @ 0x%06lX, %lu KB",
            running->label,
            (unsigned long)running->address,
            (unsigned long)(running->size / 1024u));
    }
}

void hp_get_psram_size(char* buffer, size_t length)
{
    const size_t size = esp_psram_get_size();

    if (size == 0u)
    {
        snprintf(buffer, length, "none");
    }
    else
    {
        snprintf(buffer, length, "%u MB", (unsigned)(size / (1024u * 1024u)));
    }
}

void hp_get_psram_mode(char* buffer, size_t length)
{
#if CONFIG_SPIRAM_MODE_OCT
    strlcpy(buffer, "Octal", length);
#elif CONFIG_SPIRAM
    strlcpy(buffer, "Quad", length);
#else
    strlcpy(buffer, HTTP_PANEL_PROVIDERS_EM_DASH, length);
#endif
}

void hp_get_psram_speed(char* buffer, size_t length)
{
#if CONFIG_SPIRAM
    snprintf(buffer, length, "%u MHz", (unsigned)CONFIG_SPIRAM_SPEED);
#else
    strlcpy(buffer, HTTP_PANEL_PROVIDERS_EM_DASH, length);
#endif
}

void hp_get_psram_malloc_threshold(char* buffer, size_t length)
{
#if CONFIG_SPIRAM_USE_MALLOC
    snprintf(
        buffer,
        length,
        "above %u KB",
        (unsigned)(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL / 1024u));
#else
    strlcpy(buffer, "never (heap_caps_malloc only)", length);
#endif
}

void hp_get_psram_internal_reserve(char* buffer, size_t length)
{
#if CONFIG_SPIRAM_USE_MALLOC
    snprintf(
        buffer, length, "%u KB", (unsigned)(CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL / 1024u));
#else
    strlcpy(buffer, HTTP_PANEL_PROVIDERS_EM_DASH, length);
#endif
}

/* ---- Sensors ------------------------------------------------------------ */

void hp_get_lux(char* buffer, size_t length)
{
    ltr308_sample_t sample = {0};

    if (sensor_store_get(SENSOR_ID_LTR308, &sample, sizeof(sample)) == ESP_OK)
    {
        snprintf(buffer, length, "%.1f lux", (double)sample.lux);
    }
    else
    {
        snprintf(buffer, length, HTTP_PANEL_PROVIDERS_EM_DASH);
    }
}

/* ---- System actions ----------------------------------------------------- */

void hp_action_reboot(void)
{
    esp_restart();
}

void hp_action_factory_reset(void)
{
    system_factory_reset();
}

/* ---- OTA ------------------------------------------------------------------ */

void hp_get_ota_version(char* buffer, size_t length)
{
    strlcpy(buffer, ota_get_running_version(), length);
}

void hp_get_ota_next_partition(char* buffer, size_t length)
{
    const esp_partition_t* const next = esp_ota_get_next_update_partition(NULL);

    if (next == NULL)
    {
        snprintf(buffer, length, HTTP_PANEL_PROVIDERS_EM_DASH);
    }
    else
    {
        snprintf(
            buffer,
            length,
            "%s @ 0x%06lX, %lu KB",
            next->label,
            (unsigned long)next->address,
            (unsigned long)(next->size / 1024u));
    }
}

void hp_get_ota_image_state(char* buffer, size_t length)
{
    esp_ota_img_states_t state           = ESP_OTA_IMG_UNDEFINED;
    const esp_partition_t* const running = esp_ota_get_running_partition();

    /* A USB-flashed image has no otadata entry, so it reports not found. */
    if ((running == NULL) || (esp_ota_get_state_partition(running, &state) != ESP_OK))
    {
        strlcpy(buffer, "no OTA record (USB flashed)", length);
    }
    else
    {
        strlcpy(buffer, http_panel_providers_ota_state_name(state), length);
    }
}

void hp_action_ota_reboot(void)
{
    ota_status_t status = {0};
    ota_get_status(&status);

    /* Guards against a stray/late WS "reboot" interrupting an upload still in
     * RECEIVING, or restarting the device while idle. */
    if (status.state == OTA_STATE_PENDING_REBOOT)
    {
        esp_restart();
    }
}

/**** END OF FILE ****/
