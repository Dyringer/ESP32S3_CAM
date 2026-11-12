/**
 ******************************************************************************
 * @file    smart_io_hex.c
 * @author  Daniel Dyringer
 * @date    2026-11-04
 * @brief   Hex text <-> bytes for bus transfers (JSON and shell).
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "smart_io_hex.h"

#include <stdbool.h>
#include <string.h>


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char smart_io_hex_digits[] = "0123456789ABCDEF";


/* Private functions ------------------------------------------------------- */

static bool smart_io_hex_nibble(char character, uint8_t* nibble)
{
    bool valid = true;

    if ((character >= '0') && (character <= '9'))
    {
        *nibble = (uint8_t)(character - '0');
    }
    else if ((character >= 'a') && (character <= 'f'))
    {
        *nibble = (uint8_t)((character - 'a') + 10);
    }
    else if ((character >= 'A') && (character <= 'F'))
    {
        *nibble = (uint8_t)((character - 'A') + 10);
    }
    else
    {
        valid = false;
    }

    return valid;
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t smart_io_hex_decode(const char* text, uint8_t* bytes, size_t capacity, size_t* length)
{
    esp_err_t error_code     = ESP_OK;
    const size_t text_length = strlen(text);

    if ((text_length % 2u) != 0u)
    {
        error_code = ESP_ERR_INVALID_ARG;
    }
    else if ((text_length / 2u) > capacity)
    {
        error_code = ESP_ERR_INVALID_SIZE;
    }
    else
    {
        for (size_t i = 0u; (i < (text_length / 2u)) && (error_code == ESP_OK); i++)
        {
            uint8_t high = 0u;
            uint8_t low  = 0u;

            if (smart_io_hex_nibble(text[2u * i], &high)
                && smart_io_hex_nibble(text[(2u * i) + 1u], &low))
            {
                bytes[i] = (uint8_t)((uint32_t)high << 4u) | low;
            }
            else
            {
                error_code = ESP_ERR_INVALID_ARG;
            }
        }
    }

    if (error_code == ESP_OK)
    {
        *length = text_length / 2u;
    }

    return error_code;
}

esp_err_t smart_io_hex_encode(const uint8_t* bytes, size_t length, char* text, size_t size)
{
    esp_err_t error_code = ESP_OK;

    if ((size == 0u) || (length > ((size - 1u) / 2u)))
    {
        error_code = ESP_ERR_INVALID_SIZE;
    }
    else
    {
        for (size_t i = 0u; i < length; i++)
        {
            text[2u * i]        = smart_io_hex_digits[(uint32_t)bytes[i] >> 4u];
            text[(2u * i) + 1u] = smart_io_hex_digits[(uint32_t)bytes[i] & 0x0Fu];
        }

        text[2u * length] = '\0';
    }

    return error_code;
}

/**** END OF FILE ****/
