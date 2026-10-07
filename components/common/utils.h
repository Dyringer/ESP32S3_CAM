/**
 ******************************************************************************
 * @file    utils.h
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   Common utility macros shared across all components.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef UTILS_H_
#define UTILS_H_

/* Includes ---------------------------------------------------------------- */

#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

/** @brief Safe MIN/MAX using GCC statement expressions (no double evaluation). */
#define MIN(a, b)        ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
#define MAX(a, b)        ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })
#define CLAMP(x, lo, hi) MIN(MAX((x), (lo)), (hi))

/** @brief Number of elements in a stack-allocated array. */
#define ARRAY_SIZE(arr)  (sizeof(arr) / sizeof((arr)[0]))

/** @brief Compile-time string helpers. */
#define STRINGIFY(x)     #x
#define TOSTRING(x)      STRINGIFY(x)

#define ARG_UNUSED(x)    ((void)(x))

// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */


#endif /* UTILS_H_ */

/**** END OF FILE ****/