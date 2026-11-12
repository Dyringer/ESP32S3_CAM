#include "hash.h"
#include "unity.h"

void setUp(void) {}

void tearDown(void) {}

void test_hash_string_empty_is_offset_basis(void)
{
    TEST_ASSERT_EQUAL_UINT32(HASH_FNV_OFFSET_BASIS, hash_string(""));
}

void test_hash_string_matches_fnv1a_reference(void)
{
    /* FNV-1a 32-bit of "a" */
    TEST_ASSERT_EQUAL_UINT32(0xE40C292Cu, hash_string("a"));
}

void test_hash_macro_equals_runtime(void)
{
    TEST_ASSERT_EQUAL_UINT32(hash_string("camera"), HASH_STRING("camera"));
}
