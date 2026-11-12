/**
 ******************************************************************************
 * @file    http_panel_providers.h
 * @author  Daniel Dyringer
 * @date    2026-03-09
 * @brief   HTTP panel providers — formatted device values and actions
 *          shared by the panel pages (dashboard rows, CPU load, reboot...).
 *
 *          All getters are safe to call from any task.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef HTTP_PANEL_PROVIDERS_H_
#define HTTP_PANEL_PROVIDERS_H_

/* Includes ---------------------------------------------------------------- */
#include <stddef.h>

#include "cJSON.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define HTTP_PANEL_PROVIDER_VALUE_LEN   48u

// clang-format on

/* Types ------------------------------------------------------------------- */

typedef void (*hp_value_fn_t)(char* buffer, size_t length);

/* Exposed API ------------------------------------------------------------- */

/* ---- Runtime status ----------------------------------------------------- */

void hp_get_uptime(char* buffer, size_t length);
void hp_add_cpu_load(cJSON* object);
void hp_get_wifi_state(char* buffer, size_t length);
void hp_get_task_count(char* buffer, size_t length);
void hp_get_reset_reason(char* buffer, size_t length);

/* ---- Device configuration (static for the lifetime of the image) -------- */

void hp_get_board(char* buffer, size_t length);
void hp_get_chip(char* buffer, size_t length);
void hp_get_chip_revision(char* buffer, size_t length);
void hp_get_core_count(char* buffer, size_t length);
void hp_get_cpu_freq(char* buffer, size_t length);
void hp_get_tick_rate(char* buffer, size_t length);
void hp_get_flash_size(char* buffer, size_t length);
void hp_get_idf_version(char* buffer, size_t length);
void hp_get_build_date(char* buffer, size_t length);
void hp_get_partition(char* buffer, size_t length);

/* ---- Memory chips ------------------------------------------------------- */

void hp_get_flash_id(char* buffer, size_t length);
void hp_get_flash_mode(char* buffer, size_t length);
void hp_get_flash_speed(char* buffer, size_t length);
void hp_get_app_partition(char* buffer, size_t length);
void hp_get_psram_size(char* buffer, size_t length);
void hp_get_psram_mode(char* buffer, size_t length);
void hp_get_psram_speed(char* buffer, size_t length);
void hp_get_psram_malloc_threshold(char* buffer, size_t length);
void hp_get_psram_internal_reserve(char* buffer, size_t length);

/* ---- Sensors ------------------------------------------------------------ */

void hp_get_lux(char* buffer, size_t length);

/* ---- System actions ----------------------------------------------------- */

void hp_action_reboot(void);
void hp_action_factory_reset(void);

/* ---- OTA ------------------------------------------------------------------ */

void hp_get_ota_version(char* buffer, size_t length);
void hp_get_ota_next_partition(char* buffer, size_t length);
void hp_get_ota_image_state(char* buffer, size_t length);
void hp_action_ota_reboot(void);

#endif /* HTTP_PANEL_PROVIDERS_H_ */

/**** END OF FILE ****/
