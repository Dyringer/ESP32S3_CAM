/**
 ******************************************************************************
 * @file    http_panel_providers.h
 * @author  Daniel Dyringer
 * @date    2026-03-09
 * @brief   HTTP panel providers — formatted device values and actions
 *          shared by the panel pages (dashboard rows, CPU load, reboot...).
 *
 *          Feature pages bind directly to their own subsystem's API
 *          (ota_page -> ota.h, camera_page -> camera.h); providers holds the
 *          cross-cutting data so pages do not each re-implement it.
 *
 *          Pages call these functions directly.  No function pointers or
 *          registration tables are used.
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

/** @brief Size of the buffer passed to every hp_get_* function. */
#define HTTP_PANEL_PROVIDER_VALUE_LEN   48u

// clang-format on

/* Types ------------------------------------------------------------------- */

/**
 * @brief Signature of a read provider: fills @p buf (length @p len) with a
 *        NUL-terminated string representation of the current value.
 *        Safe to call from any FreeRTOS task.
 */
typedef void (*hp_value_fn_t)(char *buf, size_t len);

/* Exposed API ------------------------------------------------------------- */

/* ---- Runtime status ----------------------------------------------------- */

/** @brief Fill @p buf with current uptime as "Xh XXm XXs". */
void hp_get_uptime(char *buf, size_t len);

/** @brief Fill @p buf with free internal heap size in bytes. */
void hp_get_heap(char *buf, size_t len);

/** @brief Fill @p buf with the lowest free heap seen since boot. */
void hp_get_heap_min(char *buf, size_t len);

/** @brief Fill @p buf with free / total PSRAM, or "none" if absent. */
void hp_get_psram(char *buf, size_t len);

/**
 * @brief Add "cpu": [<core0 %>, <core1 %>, ...] to @p obj — load over the
 *        last second, -1 while the first window is still measuring.
 */
void hp_add_cpu_load(cJSON *obj);

/** @brief Fill @p buf with the WiFi connection state name. */
void hp_get_wifi_state(char *buf, size_t len);

/** @brief Fill @p buf with the connected SSID. */
void hp_get_ssid(char *buf, size_t len);

/** @brief Fill @p buf with RSSI in dBm, or em-dash if not connected. */
void hp_get_rssi(char *buf, size_t len);

/** @brief Fill @p buf with the station IP address. */
void hp_get_ip(char *buf, size_t len);

/* ---- Device configuration (static for the lifetime of the image) -------- */

/** @brief Fill @p buf with the active board name (BOARD_NAME in bsp.h). */
void hp_get_board(char *buf, size_t len);

/** @brief Fill @p buf with chip model, revision and core count. */
void hp_get_chip(char *buf, size_t len);

/** @brief Fill @p buf with the main flash chip size. */
void hp_get_flash_size(char *buf, size_t len);

/** @brief Fill @p buf with the station MAC address. */
void hp_get_mac(char *buf, size_t len);

/** @brief Fill @p buf with the ESP-IDF version the image was built with. */
void hp_get_idf_version(char *buf, size_t len);

/** @brief Fill @p buf with the image build date and time. */
void hp_get_build_date(char *buf, size_t len);

/** @brief Fill @p buf with the label of the running app partition. */
void hp_get_partition(char *buf, size_t len);

/** @brief Fill @p buf with the camera sensor name, or "not detected". */
void hp_get_camera(char *buf, size_t len);

/* ---- System actions ----------------------------------------------------- */

/** @brief Restart the device. Does not return. */
void hp_action_reboot(void);

/** @brief Erase WiFi credentials and restart. Does not return. */
void hp_action_factory_reset(void);

/* ---- OTA ------------------------------------------------------------------ */

/** @brief Fill @p buf with the currently running firmware version string. */
void hp_get_ota_version(char *buf, size_t len);

/**
 * @brief Restart the device to boot the newly applied OTA image.
 *        No-op unless an update is actually staged and waiting
 *        (ota_get_status()->state == OTA_STATE_PENDING_REBOOT) — does not
 *        return if it does restart.
 */
void hp_action_ota_reboot(void);

#endif /* HTTP_PANEL_PROVIDERS_H_ */

/**** END OF FILE ****/
