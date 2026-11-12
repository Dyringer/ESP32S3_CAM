/**
 ******************************************************************************
 * @file    hash.c
 * @author  Daniel Dyringer
 * @date    2026-10-11
 * @brief   32-bit FNV-1a string hash, at runtime and at compile time.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "hash.h"

#include <stddef.h>


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */


/* Private functions ------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

uint32_t hash_string(const char* string)
{
    uint32_t hash = HASH_FNV_OFFSET_BASIS;

    for (size_t i = 0u; (string != NULL) && (string[i] != '\0'); i++)
    {
        hash ^= (uint32_t)(uint8_t)string[i];
        hash *= HASH_FNV_PRIME;
    }

    return hash;
}

/**** END OF FILE ****/
