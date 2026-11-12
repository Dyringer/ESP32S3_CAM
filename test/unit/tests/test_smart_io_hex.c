#include <string.h>

#include "smart_io_hex.h"
#include "unity.h"

void setUp(void) {}

void tearDown(void) {}

void test_smart_io_hex_decode_mixed_case(void)
{
    const uint8_t expected[] = {0x00u, 0xD0u, 0xafu, 0x7Fu};
    uint8_t bytes[8]         = {0};
    size_t length            = 0u;

    TEST_ASSERT_EQUAL(
        ESP_OK, smart_io_hex_decode("00d0Af7F", bytes, sizeof(bytes), &length));
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), length);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, bytes, sizeof(expected));
}

void test_smart_io_hex_decode_empty_is_zero_bytes(void)
{
    uint8_t bytes[1] = {0};
    size_t length    = 99u;

    TEST_ASSERT_EQUAL(ESP_OK, smart_io_hex_decode("", bytes, sizeof(bytes), &length));
    TEST_ASSERT_EQUAL_size_t(0u, length);
}

void test_smart_io_hex_decode_refuses_odd_length(void)
{
    uint8_t bytes[4] = {0};
    size_t length    = 7u;

    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG, smart_io_hex_decode("abc", bytes, sizeof(bytes), &length));
    TEST_ASSERT_EQUAL_size_t(7u, length);
}

void test_smart_io_hex_decode_refuses_non_hex(void)
{
    uint8_t bytes[4] = {0};
    size_t length    = 0u;

    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG, smart_io_hex_decode("0g", bytes, sizeof(bytes), &length));
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG, smart_io_hex_decode("a b ", bytes, sizeof(bytes), &length));
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG, smart_io_hex_decode("0x12", bytes, sizeof(bytes), &length));
}

void test_smart_io_hex_decode_refuses_more_than_capacity(void)
{
    uint8_t bytes[3] = {0xEEu, 0xEEu, 0xEEu};
    size_t length    = 0u;

    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_SIZE, smart_io_hex_decode("01020304", bytes, 2u, &length));
    TEST_ASSERT_EQUAL_HEX8(0xEEu, bytes[0]);
    TEST_ASSERT_EQUAL(ESP_OK, smart_io_hex_decode("0102", bytes, 2u, &length));
    TEST_ASSERT_EQUAL_HEX8(0xEEu, bytes[2]);
}

void test_smart_io_hex_encode_upper_case(void)
{
    const uint8_t bytes[] = {0x5Au, 0x3Cu, 0x00u, 0xffu};
    char text[9]          = {0};

    TEST_ASSERT_EQUAL(
        ESP_OK, smart_io_hex_encode(bytes, sizeof(bytes), text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("5A3C00FF", text);
}

void test_smart_io_hex_encode_needs_room_for_terminator(void)
{
    const uint8_t bytes[] = {0x01u, 0x02u};
    char text[5]          = {0};

    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_SIZE, smart_io_hex_encode(bytes, sizeof(bytes), text, 4u));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, smart_io_hex_encode(bytes, 0u, text, 0u));
    TEST_ASSERT_EQUAL(ESP_OK, smart_io_hex_encode(bytes, sizeof(bytes), text, 5u));
    TEST_ASSERT_EQUAL_STRING("0102", text);
}

void test_smart_io_hex_round_trip(void)
{
    uint8_t bytes[256]   = {0};
    uint8_t decoded[256] = {0};
    char text[513]       = {0};
    size_t length        = 0u;

    for (size_t i = 0u; i < sizeof(bytes); i++)
    {
        bytes[i] = (uint8_t)i;
    }

    TEST_ASSERT_EQUAL(
        ESP_OK, smart_io_hex_encode(bytes, sizeof(bytes), text, sizeof(text)));
    TEST_ASSERT_EQUAL(ESP_OK, smart_io_hex_decode(text, decoded, sizeof(decoded), &length));
    TEST_ASSERT_EQUAL_size_t(sizeof(bytes), length);
    TEST_ASSERT_EQUAL_MEMORY(bytes, decoded, sizeof(bytes));
}
