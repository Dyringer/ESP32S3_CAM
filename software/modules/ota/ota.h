/**
 ******************************************************************************
 * @file    ota.h
 * @author  Daniel Dyringer
 * @date    2026-08-13
 * @brief   OTA core — push-style firmware update wrapper around esp_ota_ops.
 *
 *          Never reboots on success — the caller decides when, so a client
 *          can observe "100%" before the device drops off the network.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef OTA_H_
#define OTA_H_

/* Includes ---------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */

ESP_EVENT_DECLARE_BASE(OTA_EVENTS);

/* event_data is an ota_status_t snapshot. ota_abort() also posts FAILED. */
typedef enum ota_event_e
{
    OTA_EVENT_PROGRESS = 0,
    OTA_EVENT_DONE     = 1,
    OTA_EVENT_FAILED   = 2,
} ota_event_t;

typedef enum ota_state_e
{
    OTA_STATE_IDLE           = 0,
    OTA_STATE_RECEIVING      = 1,
    OTA_STATE_VALIDATING     = 2,
    OTA_STATE_APPLYING       = 3,
    OTA_STATE_PENDING_REBOOT = 4,
    OTA_STATE_FAILED         = 5,
} ota_state_t;

typedef struct ota_status_s
{
    ota_state_t state;
    size_t bytes_written;
    size_t bytes_expected; /**< From Content-Length; 0 if unknown. */
    esp_err_t last_error;
} ota_status_t;

/* Exposed API ------------------------------------------------------------- */

esp_err_t ota_init(void);
esp_err_t ota_begin(size_t image_size_hint);
esp_err_t ota_write(const uint8_t* data, size_t length);
esp_err_t ota_finish(void);
esp_err_t ota_abort(void);

void ota_get_status(ota_status_t* status);
const char* ota_get_running_version(void);

#endif /* OTA_H_ */

/**** END OF FILE ****/
