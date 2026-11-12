/**
 ******************************************************************************
 * @file    hash.h
 * @author  Daniel Dyringer
 * @date    2026-10-11
 * @brief   32-bit FNV-1a string hash, at runtime and at compile time.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef HASH_H_
#define HASH_H_

/* Includes ---------------------------------------------------------------- */
#include <stdint.h>

/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* http://www.isthe.com/chongo/tech/comp/fnv/ */
#define HASH_FNV_OFFSET_BASIS   2166136261u
#define HASH_FNV_PRIME          16777619u

/* HASH_STRING("literal") equals hash_string("literal") and is folded by GCC
 * into a constant, also at -Og. Literals only, up to HASH_STRING_MAX_LENGTH
 * characters. It is not an integer constant expression (C forbids indexing a
 * string literal there), so it cannot be a case label or a _Static_assert.
 *
 * Each step uses the running hash once, so the expansion stays linear. Past
 * the terminator a step is (h ^ 0) * 1, i.e. a no-op. */
#define HASH_STRING_MAX_LENGTH  32u

#define HASH_IN(s, i)           ((i) < (sizeof(s) - 1u))
#define HASH_INDEX(s, i)        (HASH_IN(s, i) ? (i) : 0u)
#define HASH_CHAR(s, i)         \
    (HASH_IN(s, i) ? (uint32_t)(uint8_t)(s)[HASH_INDEX(s, i)] : 0u)
#define HASH_FACTOR(s, i)       (HASH_IN(s, i) ? HASH_FNV_PRIME : 1u)
#define HASH_STEP(s, i, h)      (((h) ^ HASH_CHAR(s, i)) * HASH_FACTOR(s, i))
#define HASH_STEP4(s, i, h)     HASH_STEP(s, (i) + 3u, HASH_STEP(s, (i) + 2u,     \
                                HASH_STEP(s, (i) + 1u, HASH_STEP(s, (i), h))))
#define HASH_STEP16(s, i, h)    HASH_STEP4(s, (i) + 12u, HASH_STEP4(s, (i) + 8u,  \
                                HASH_STEP4(s, (i) + 4u, HASH_STEP4(s, (i), h))))

#define HASH_STRING(s)                                                          \
    ((uint32_t)HASH_STEP16(s, 16u, HASH_STEP16(s, 0u, HASH_FNV_OFFSET_BASIS)))

// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

uint32_t hash_string(const char* string);

#endif /* HASH_H_ */

/**** END OF FILE ****/
