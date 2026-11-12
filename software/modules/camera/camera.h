/**
 ******************************************************************************
 * @file    camera.h
 * @author  Daniel Dyringer
 * @date    2026-09-30
 * @brief   Camera core — OV3660 bring-up, frame access and illumination LED,
 *          on top of espressif/esp32-camera.
 *
 *          The sensor outputs JPEG directly, so a frame is a complete JPEG
 *          image — nothing is encoded on the ESP32.
 *
 *          Settings live in camera_settings; camera_init() registers them,
 *          applies the stored set and every later change. data_manager must
 *          be initialised before camera_init().
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


// clang-format on

/* Types ------------------------------------------------------------------- */

typedef struct camera_frame_s
{
    const uint8_t* buffer; /**< JPEG data.                              */
    size_t length;         /**< JPEG length in bytes.                   */
    uint16_t width;
    uint16_t height;
    void* driver_frame; /**< Driver handle — do not touch.           */
} camera_frame_t;

/* Exposed API ------------------------------------------------------------- */

esp_err_t camera_init(void);
bool camera_is_ready(void);
const char* camera_sensor_name(void);

esp_err_t camera_framesize_resolution(int framesize, uint16_t* width, uint16_t* height);

/* ---- Frames ---- */

esp_err_t camera_frame_acquire(camera_frame_t* frame);
esp_err_t camera_still_acquire(camera_frame_t* frame);

void camera_frame_release(camera_frame_t* frame);

/* ---- Light ---- */

void camera_light_enable(bool on);

#endif /* CAMERA_H_ */

/**** END OF FILE ****/
