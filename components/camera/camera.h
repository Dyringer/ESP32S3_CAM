/**
 ******************************************************************************
 * @file    camera.h
 * @author  Daniel Dyringer
 * @date    2026-09-30
 * @brief   Camera core — OV3660 bring-up, runtime settings, frame access and
 *          illumination LED, on top of espressif/esp32-camera.
 *
 *          Knows nothing about HTTP. The http_panel camera page and the
 *          "camera" shell command are both thin front-ends over this API.
 *
 *          The sensor outputs JPEG directly, so a frame is a complete JPEG
 *          image — nothing is encoded on the ESP32.
 *
 *          Every sensor access is serialised by an internal mutex, so the
 *          setters are safe to call from any task (httpd, console, ...).
 *
 *          Settings changed at runtime are not persistent until
 *          camera_settings_save() stores them in NVS; camera_init() re-applies
 *          the saved set on every boot.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef CAMERA_H_
#define CAMERA_H_

/* Includes ---------------------------------------------------------------- */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

/** @brief Name of the setting that selects the output resolution. */
#define CAMERA_SETTING_FRAMESIZE    "framesize"

// clang-format on

/* Types ------------------------------------------------------------------- */

/** @brief Static description of one runtime setting. */
typedef struct
{
    const char *name; /**< Setting key, e.g. "brightness".               */
    int         min;  /**< Lowest accepted value (inclusive).             */
    int         max;  /**< Highest accepted value (inclusive).            */
} camera_setting_desc_t;

/** @brief One captured JPEG frame. Must be released after use. */
typedef struct
{
    const uint8_t *buf;          /**< JPEG data.                              */
    size_t         len;          /**< JPEG length in bytes.                   */
    uint16_t       width;
    uint16_t       height;
    void          *priv;         /**< Driver handle — do not touch.           */
} camera_frame_t;

/* Exposed API ------------------------------------------------------------- */

/**
 * @brief Initialise the camera driver, probe the sensor and apply the boot
 *        defaults (HD 1280x720 JPEG, OV3660 orientation/colour fixes), and
 *        set up the LED PWM.
 *
 *        Frame buffers are sized for the sensor's largest resolution so the
 *        resolution can later be raised at runtime without truncated frames.
 *
 * @return ESP_OK on success. On failure the rest of the API reports
 *         ESP_ERR_INVALID_STATE and the firmware keeps running without video.
 */
esp_err_t camera_init(void);

/** @brief True once camera_init() has succeeded. */
bool camera_is_ready(void);

/** @brief Detected sensor name (e.g. "OV3660"), or "not detected". */
const char *camera_sensor_name(void);

/* ---- Settings ----------------------------------------------------------- */

/** @brief Number of runtime settings (sensor settings + LED). */
size_t camera_setting_count(void);

/** @brief Description of setting @p index, or NULL if out of range. */
const camera_setting_desc_t *camera_setting_at(size_t index);

/**
 * @brief Apply one setting.
 *
 * @return ESP_OK, ESP_ERR_NOT_FOUND (unknown name), ESP_ERR_INVALID_ARG
 *         (value out of range), ESP_ERR_INVALID_STATE (camera not ready) or
 *         ESP_FAIL (sensor rejected the write).
 */
esp_err_t camera_setting_set(const char *name, int value);

/** @brief Read the current value of one setting. Same errors as set. */
esp_err_t camera_setting_get(const char *name, int *value);

/**
 * @brief Store every current setting in NVS; camera_init() re-applies them
 *        on the next boot. Clears the "unsaved" flag.
 */
esp_err_t camera_settings_save(void);

/**
 * @brief Re-apply the boot defaults to every setting and erase the saved
 *        set from NVS. Clears the "unsaved" flag.
 */
esp_err_t camera_settings_restore_defaults(void);

/** @brief True if any live setting differs from what the next boot applies. */
bool camera_settings_unsaved(void);

/**
 * @brief Resolution of framesize value @p framesize.
 *
 * @return ESP_OK, or ESP_ERR_INVALID_ARG if it is outside the range of
 *         the "framesize" setting.
 */
esp_err_t camera_framesize_resolution(int framesize, uint16_t *width, uint16_t *height);

/* ---- Frames ------------------------------------------------------------- */

/**
 * @brief Borrow the latest frame from the driver. Blocks until one is ready.
 *        Pair every successful call with camera_frame_release().
 */
esp_err_t camera_frame_acquire(camera_frame_t *frame);

/**
 * @brief Like camera_frame_acquire(), but for a single still: if the LED
 *        intensity is non-zero and the LED is not already lit, it is
 *        flashed around the capture so the exposure includes it.
 */
esp_err_t camera_still_acquire(camera_frame_t *frame);

/** @brief Return a frame to the driver. Safe to call on an empty frame. */
void camera_frame_release(camera_frame_t *frame);

/* ---- LED ---------------------------------------------------------------- */

/**
 * @brief Switch the LED on (at the "led_intensity" duty) or off.
 *        Intensity changes while on take effect immediately. Used by the
 *        stream to light the scene while anyone is watching.
 */
void camera_led_enable(bool on);

/* ---- Raw register access (bring-up / tuning) ---------------------------- */

/** @brief Read sensor register @p reg masked by @p mask into @p value. */
esp_err_t camera_reg_get(int reg, int mask, int *value);

/** @brief Read-modify-write the @p mask bits of sensor register @p reg. */
esp_err_t camera_reg_set(int reg, int mask, int value);

#endif /* CAMERA_H_ */

/**** END OF FILE ****/
