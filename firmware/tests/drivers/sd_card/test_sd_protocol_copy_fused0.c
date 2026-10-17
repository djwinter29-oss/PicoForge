#include "unity.h"
void pf_sd_lock(void) {}
void pf_sd_unlock(void) {}

#define PF_SD_NATIVE_FUSED_COPY 0

#include "../../../src/drivers/sd_card/sd_protocol.c"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void reference_crc(const uint8_t block[512], uint16_t crc[4])
{
    memset(crc, 0, sizeof(uint16_t) * 4u);
    for (unsigned byte = 0u; byte < 512u; ++byte) {
        for (unsigned half = 0u; half < 2u; ++half) {
            uint8_t nibble = (uint8_t)(block[byte] >> (half == 0u ? 4u : 0u));
            for (unsigned lane = 0u; lane < 4u; ++lane) {
                bool feedback = ((crc[lane] >> 15u) ^ (nibble >> lane)) & 1u;
                crc[lane] = (uint16_t)(crc[lane] << 1u);
                if (feedback) crc[lane] ^= 0x1021u;
            }
        }
    }
}

void test_nonfused_copy_matches_crc_and_round_trips_unaligned_blocks(void)
{
    uint8_t input[515], output[515], wire[523];

    for (unsigned alignment = 0u; alignment < 4u; ++alignment) {
        uint8_t *block = input + alignment;
        uint8_t *decoded = output + alignment;
        uint8_t *image = wire + alignment;
        uint16_t expected[4], actual[4];

        for (unsigned byte = 0u; byte < 512u; ++byte) {
            block[byte] = (uint8_t)(byte * 73u + alignment);
        }
        reference_crc(block, expected);
        pf_sd_native_crc16_4bit(block, actual);
        TEST_ASSERT_EQUAL_MEMORY(expected, actual, sizeof(actual));
        TEST_ASSERT_TRUE(pf_sd_native_encode_data_block(block, image));
        TEST_ASSERT_EQUAL_MEMORY(block, image, 512u);
        TEST_ASSERT_TRUE(pf_sd_native_verify_data_block(image));
        TEST_ASSERT_TRUE(pf_sd_native_decode_data_block(image, decoded));
        TEST_ASSERT_EQUAL_MEMORY(block, decoded, 512u);

        image[512] ^= 1u;
        TEST_ASSERT_FALSE(pf_sd_native_verify_data_block(image));
        TEST_ASSERT_FALSE(pf_sd_native_decode_data_block(image, decoded));
    }
}
