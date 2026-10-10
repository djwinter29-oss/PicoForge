#include "unity.h"
#include "sdio_protocol.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_crc7_matches_cmd0_reference(void)
{
    const uint8_t cmd0_header[] = { 0x40u, 0x00u, 0x00u, 0x00u, 0x00u };

    TEST_ASSERT_EQUAL_UINT8(0x4au, picoforge_sdio_crc7(cmd0_header, sizeof(cmd0_header)));
}

void test_crc7_returns_zero_for_null_and_empty_input(void)
{
    const uint8_t bytes[] = { 0xffu };

    TEST_ASSERT_EQUAL_UINT8(0u, picoforge_sdio_crc7(NULL, sizeof(bytes)));
    TEST_ASSERT_EQUAL_UINT8(0u, picoforge_sdio_crc7(bytes, 0u));
}

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

void test_builds_max_command_and_big_endian_argument(void)
{
    uint8_t frame[6];

    TEST_ASSERT_TRUE(picoforge_sdio_build_command(63u, 0x89abcdefu, frame));
    TEST_ASSERT_EQUAL_HEX8(0x7fu, frame[0]);
    TEST_ASSERT_EQUAL_HEX8(0x89u, frame[1]);
    TEST_ASSERT_EQUAL_HEX8(0xabu, frame[2]);
    TEST_ASSERT_EQUAL_HEX8(0xcdu, frame[3]);
    TEST_ASSERT_EQUAL_HEX8(0xefu, frame[4]);
    TEST_ASSERT_EQUAL_HEX8(1u, frame[5] & 1u);
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

void test_parses_response_without_crc_check_when_requested(void)
{
    uint8_t response[6] = { 0x11u, 0x12u, 0x34u, 0x56u, 0x78u, 0x01u };
    uint32_t argument;

    TEST_ASSERT_TRUE(picoforge_sdio_parse_response(17u, response, &argument, false));
    TEST_ASSERT_EQUAL_HEX32(0x12345678u, argument);
}

void test_parses_r3_ocr_without_a_command_index(void)
{
    uint8_t response[6] = { 0x3fu, 0xc0u, 0xffu, 0x80u, 0x00u, 0x01u };
    uint32_t ocr;

    TEST_ASSERT_TRUE(picoforge_sdio_parse_r3_response(response, &ocr));
    TEST_ASSERT_EQUAL_HEX32(0xc0ff8000u, ocr);
    TEST_ASSERT_FALSE(picoforge_sdio_parse_r3_response(NULL, &ocr));
    TEST_ASSERT_FALSE(picoforge_sdio_parse_r3_response(response, NULL));
    response[0] |= 0x40u;
    TEST_ASSERT_FALSE(picoforge_sdio_parse_r3_response(response, &ocr));
    response[0] &= 0xbfu;
    response[5] = 0u;
    TEST_ASSERT_FALSE(picoforge_sdio_parse_r3_response(response, &ocr));
}

void test_unpacks_partial_response_words_from_the_low_bits(void)
{
    const uint32_t r1_words[] = { 0x01020304u, 0x00000506u };
    const uint8_t r1_expected[] = { 1u, 2u, 3u, 4u, 5u, 6u };
    const uint32_t r2_words[] = { 0x01020304u, 0x05060708u, 0x090a0b0cu,
                                  0x0d0e0f10u, 0x00000011u };
    uint8_t r1[6];
    uint8_t r2[17];

    TEST_ASSERT_TRUE(picoforge_sdio_response_words_to_bytes(r1_words, 48u, r1, sizeof(r1)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(r1_expected, r1, sizeof(r1));
    TEST_ASSERT_TRUE(picoforge_sdio_response_words_to_bytes(r2_words, 136u, r2, sizeof(r2)));
    TEST_ASSERT_EQUAL_HEX8(0x11u, r2[16]);
    TEST_ASSERT_FALSE(picoforge_sdio_response_words_to_bytes(r1_words, 48u, r1, 5u));
    TEST_ASSERT_FALSE(picoforge_sdio_response_words_to_bytes(r1_words, 47u, r1, sizeof(r1)));
}

void test_classifies_cmd8_v2_and_legacy_cards(void)
{
    bool supports_v2;

    TEST_ASSERT_TRUE(picoforge_sdio_cmd8_supports_v2(0x000001aau, &supports_v2));
    TEST_ASSERT_TRUE(supports_v2);
    TEST_ASSERT_TRUE(picoforge_sdio_cmd8_supports_v2(1u << 22u, &supports_v2));
    TEST_ASSERT_FALSE(supports_v2);
    TEST_ASSERT_FALSE(picoforge_sdio_cmd8_supports_v2(0x000001abu, &supports_v2));
    TEST_ASSERT_FALSE(picoforge_sdio_cmd8_supports_v2(1u << 22u, NULL));
}

void test_decodes_data_write_response_from_low_fifo_nibble(void)
{
    TEST_ASSERT_TRUE(picoforge_sdio_write_response_accepted(0x00000005u));
    TEST_ASSERT_FALSE(picoforge_sdio_write_response_accepted(0x50000000u));
    TEST_ASSERT_FALSE(picoforge_sdio_write_response_accepted(0x0000000bu));
}

void test_rejects_malformed_short_response_frames(void)
{
    uint8_t response[6] = { 0x11u, 0u, 0u, 0u, 0u, 0x01u };
    uint32_t argument;

    TEST_ASSERT_FALSE(picoforge_sdio_parse_response(17u, NULL, &argument, false));
    TEST_ASSERT_FALSE(picoforge_sdio_parse_response(17u, response, NULL, false));
    response[0] |= 0x40u;
    TEST_ASSERT_FALSE(picoforge_sdio_parse_response(17u, response, &argument, false));
    response[0] &= 0xbfu;
    response[5] = 0u;
    TEST_ASSERT_FALSE(picoforge_sdio_parse_response(17u, response, &argument, false));
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
    TEST_ASSERT_FALSE(picoforge_sdio_validate_r2_response(NULL));
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

void test_decodes_v1_csd_sector_count(void)
{
    uint8_t csd[16] = { 0u };
    uint32_t sectors;

    csd[5] = 9u;
    csd[7] = 0xffu;
    csd[8] = 0xc0u;
    TEST_ASSERT_TRUE(picoforge_sdio_csd_sector_count(csd, &sectors));
    TEST_ASSERT_EQUAL_UINT32(4096u, sectors);
}

void test_rejects_invalid_csd_inputs_and_structure(void)
{
    uint8_t csd[16] = { 0u };
    uint32_t sectors;

    TEST_ASSERT_FALSE(picoforge_sdio_csd_sector_count(NULL, &sectors));
    TEST_ASSERT_FALSE(picoforge_sdio_csd_sector_count(csd, NULL));
    csd[0] = 0x80u;
    TEST_ASSERT_FALSE(picoforge_sdio_csd_sector_count(csd, &sectors));
    csd[0] = 0xc0u;
    TEST_ASSERT_FALSE(picoforge_sdio_csd_sector_count(csd, &sectors));
}

void test_rejects_invalid_v1_block_length_and_zero_capacity(void)
{
    uint8_t csd[16] = { 0u };
    uint32_t sectors = 17u;

    csd[5] = 12u;
    TEST_ASSERT_FALSE(picoforge_sdio_csd_sector_count(csd, &sectors));
    csd[5] = 0u;
    TEST_ASSERT_FALSE(picoforge_sdio_csd_sector_count(csd, &sectors));
    TEST_ASSERT_EQUAL_UINT32(17u, sectors);
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

void test_round_trips_patterned_data_block(void)
{
    uint8_t source[512];
    uint8_t destination[512];
    uint32_t words[PICOFORGE_SDIO_TRANSFER_WORDS] = { 0u };

    for (size_t index = 0u; index < sizeof(source); index++) {
        source[index] = (uint8_t)(index * 37u + 11u);
    }
    picoforge_sdio_pack_data_block(source, words);
    TEST_ASSERT_TRUE(picoforge_sdio_unpack_data_block(words, destination));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(source, destination, sizeof(source));
}

void test_packs_data_nibbles_in_wire_order(void)
{
    uint8_t source[512] = { 0u };
    uint32_t words[PICOFORGE_SDIO_TRANSFER_WORDS];

    memset(words, 0xff, sizeof(words));
    source[0] = 0x12u;
    source[1] = 0x34u;
    source[2] = 0x56u;
    source[3] = 0x78u;
    picoforge_sdio_pack_data_block(source, words);

    TEST_ASSERT_EQUAL_HEX32(0x12345678u, words[0]);
    TEST_ASSERT_NOT_EQUAL(0xffffffffu, words[128]);
    TEST_ASSERT_NOT_EQUAL(0xffffffffu, words[129]);
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

void test_detects_corrupt_data_and_crc_words(void)
{
    uint8_t source[512] = { 0u };
    uint8_t destination[512];
    uint32_t words[PICOFORGE_SDIO_TRANSFER_WORDS] = { 0u };

    picoforge_sdio_pack_data_block(source, words);
    words[0] ^= 0x10000000u;
    TEST_ASSERT_FALSE(picoforge_sdio_unpack_data_block(words, destination));
    words[0] ^= 0x10000000u;
    words[128] ^= 1u;
    TEST_ASSERT_FALSE(picoforge_sdio_unpack_data_block(words, destination));
}

void test_handles_null_data_block_buffers(void)
{
    uint16_t crc[4];
    uint32_t words[PICOFORGE_SDIO_TRANSFER_WORDS] = { 0u };
    uint8_t block[512];

    picoforge_sdio_crc16_4bit(NULL, crc);
    picoforge_sdio_crc16_4bit(block, NULL);
    picoforge_sdio_pack_data_block(NULL, words);
    picoforge_sdio_pack_data_block(block, NULL);
    TEST_ASSERT_FALSE(picoforge_sdio_unpack_data_block(NULL, block));
    TEST_ASSERT_FALSE(picoforge_sdio_unpack_data_block(words, NULL));
}