#include "unity.h"
#include "sdio_protocol.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_builds_cmd0_frame_with_standard_crc(void)
{
    uint8_t frame[6];
    const uint8_t expected[] = { 0x40u, 0x00u, 0x00u, 0x00u, 0x00u, 0x95u };

    TEST_ASSERT_TRUE(picoforge_sdio_build_command(0u, 0u, frame));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame, sizeof(expected));
}

void test_builds_cmd8_frame_with_standard_crc(void)
{
    uint8_t frame[6];
    const uint8_t expected[] = { 0x48u, 0x00u, 0x00u, 0x01u, 0xaau, 0x87u };

    TEST_ASSERT_TRUE(picoforge_sdio_build_command(8u, 0x000001aau, frame));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame, sizeof(expected));
}

void test_rejects_invalid_command_or_output_buffer(void)
{
    uint8_t frame[6];

    TEST_ASSERT_FALSE(picoforge_sdio_build_command(64u, 0u, frame));
    TEST_ASSERT_FALSE(picoforge_sdio_build_command(0u, 0u, NULL));
}

void test_parses_response_and_checks_command_crc(void)
{
    uint8_t response[6] = { 0x08u, 0x00u, 0x00u, 0x01u, 0xaau, 0u };
    uint32_t argument;

    response[5] = (uint8_t)((picoforge_sdio_crc7(response, 5u) << 1u) | 1u);
    TEST_ASSERT_TRUE(picoforge_sdio_parse_response(8u, response, &argument, true));
    TEST_ASSERT_EQUAL_HEX32(0x000001aau, argument);
    TEST_ASSERT_FALSE(picoforge_sdio_parse_response(9u, response, &argument, true));
    response[5] ^= 2u;
    TEST_ASSERT_FALSE(picoforge_sdio_parse_response(8u, response, &argument, true));
}

void test_decodes_v2_csd_sector_count(void)
{
    uint8_t csd[16] = { 0u };
    uint32_t sectors;

    csd[0] = 0x40u;
    csd[7] = 0x03u;
    csd[8] = 0xffu;
    csd[9] = 0xffu;
    TEST_ASSERT_TRUE(picoforge_sdio_csd_sector_count(csd, &sectors));
    TEST_ASSERT_EQUAL_UINT32(0x10000000u, sectors);
}

void test_validates_r2_response_crc_and_end_bit(void)
{
    uint8_t response[17] = { 0u };

    response[0] = 0x3fu;
    response[16] = (uint8_t)((picoforge_sdio_crc7(response, 16u) << 1u) | 1u);
    TEST_ASSERT_TRUE(picoforge_sdio_validate_r2_response(response));
    response[4] ^= 1u;
    TEST_ASSERT_FALSE(picoforge_sdio_validate_r2_response(response));
    response[4] ^= 1u;
    response[16] &= 0xfeu;
    TEST_ASSERT_FALSE(picoforge_sdio_validate_r2_response(response));
}

void test_round_trips_zero_filled_data_block_and_lane_crcs(void)
{
    uint8_t source[512] = { 0u };
    uint8_t destination[512];
    uint32_t words[PICOFORGE_SDIO_TRANSFER_WORDS] = { 0u };

    picoforge_sdio_pack_data_block(source, words);
    TEST_ASSERT_TRUE(picoforge_sdio_unpack_data_block(words, destination));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(source, destination, sizeof(source));
}

void test_packs_data_nibbles_in_wire_order(void)
{
    uint8_t source[512] = { 0u };
    uint32_t words[PICOFORGE_SDIO_TRANSFER_WORDS] = { 0u };

    source[0] = 0x12u;
    source[1] = 0x34u;
    source[2] = 0x56u;
    source[3] = 0x78u;
    picoforge_sdio_pack_data_block(source, words);

    TEST_ASSERT_EQUAL_HEX32(0x12345678u, words[0]);
}

void test_calculates_known_four_lane_crc16_vector(void)
{
    uint8_t block[512];
    uint16_t crc[4];
    const uint16_t expected[4] = { 0x6aa3u, 0xa97du, 0x10b5u, 0x7357u };

    for (size_t index = 0u; index < sizeof(block); index++) {
        block[index] = (uint8_t)index;
    }
    picoforge_sdio_crc16_4bit(block, crc);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(expected, crc, 4u);
}

void test_detects_corrupt_data_block_crc(void)
{
    uint8_t source[512] = { 0u };
    uint8_t destination[512];
    uint32_t words[PICOFORGE_SDIO_TRANSFER_WORDS] = { 0u };

    picoforge_sdio_pack_data_block(source, words);
    words[128] ^= 1u;
    TEST_ASSERT_FALSE(picoforge_sdio_unpack_data_block(words, destination));
}