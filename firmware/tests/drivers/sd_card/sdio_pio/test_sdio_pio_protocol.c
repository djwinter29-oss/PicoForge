#include "unity.h"
#include "sd_card_protocol.h"
#include "sdio_pio_protocol.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_crc7_matches_cmd0_reference(void)
{
    const uint8_t cmd0_header[] = { 0x40u, 0x00u, 0x00u, 0x00u, 0x00u };

    TEST_ASSERT_EQUAL_UINT8(0x4au, pf_sdio_pio_crc7(cmd0_header, sizeof(cmd0_header)));
}

void test_crc7_returns_zero_for_null_and_empty_input(void)
{
    const uint8_t bytes[] = { 0xffu };

    TEST_ASSERT_EQUAL_UINT8(0u, pf_sdio_pio_crc7(NULL, sizeof(bytes)));
    TEST_ASSERT_EQUAL_UINT8(0u, pf_sdio_pio_crc7(bytes, 0u));
}

void test_builds_cmd0_frame_with_standard_crc(void)
{
    uint8_t frame[6];
    const uint8_t expected[] = { 0x40u, 0x00u, 0x00u, 0x00u, 0x00u, 0x95u };

    TEST_ASSERT_TRUE(pf_sdio_pio_build_command(0u, 0u, frame));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame, sizeof(expected));
}

void test_builds_cmd8_frame_with_standard_crc(void)
{
    uint8_t frame[6];
    const uint8_t expected[] = { 0x48u, 0x00u, 0x00u, 0x01u, 0xaau, 0x87u };

    TEST_ASSERT_TRUE(pf_sdio_pio_build_command(8u, 0x000001aau, frame));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame, sizeof(expected));
}

void test_builds_max_command_and_big_endian_argument(void)
{
    uint8_t frame[6];

    TEST_ASSERT_TRUE(pf_sdio_pio_build_command(63u, 0x89abcdefu, frame));
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

    TEST_ASSERT_FALSE(pf_sdio_pio_build_command(64u, 0u, frame));
    TEST_ASSERT_FALSE(pf_sdio_pio_build_command(0u, 0u, NULL));
}

void test_parses_response_and_checks_command_crc(void)
{
    uint8_t response[6] = { 0x08u, 0x00u, 0x00u, 0x01u, 0xaau, 0u };
    uint32_t argument;

    response[5] = (uint8_t)((pf_sdio_pio_crc7(response, 5u) << 1u) | 1u);
    TEST_ASSERT_TRUE(pf_sdio_pio_parse_response(8u, response, &argument, true));
    TEST_ASSERT_EQUAL_HEX32(0x000001aau, argument);
    TEST_ASSERT_FALSE(pf_sdio_pio_parse_response(9u, response, &argument, true));
    response[5] ^= 2u;
    TEST_ASSERT_FALSE(pf_sdio_pio_parse_response(8u, response, &argument, true));
}

void test_parses_response_without_crc_check_when_requested(void)
{
    uint8_t response[6] = { 0x11u, 0x12u, 0x34u, 0x56u, 0x78u, 0x01u };
    uint32_t argument;

    TEST_ASSERT_TRUE(pf_sdio_pio_parse_response(17u, response, &argument, false));
    TEST_ASSERT_EQUAL_HEX32(0x12345678u, argument);
}

void test_parses_r3_ocr_without_a_command_index(void)
{
    uint8_t response[6] = { 0x3fu, 0xc0u, 0xffu, 0x80u, 0x00u, 0x01u };
    uint32_t ocr;

    TEST_ASSERT_TRUE(pf_sdio_pio_parse_r3_response(response, &ocr));
    TEST_ASSERT_EQUAL_HEX32(0xc0ff8000u, ocr);
    TEST_ASSERT_FALSE(pf_sdio_pio_parse_r3_response(NULL, &ocr));
    TEST_ASSERT_FALSE(pf_sdio_pio_parse_r3_response(response, NULL));
    response[0] |= 0x40u;
    TEST_ASSERT_FALSE(pf_sdio_pio_parse_r3_response(response, &ocr));
    response[0] &= 0xbfu;
    response[5] = 0u;
    TEST_ASSERT_FALSE(pf_sdio_pio_parse_r3_response(response, &ocr));
}

void test_unpacks_partial_response_words_from_the_low_bits(void)
{
    const uint32_t r1_words[] = { 0x01020304u, 0x00000506u };
    const uint8_t r1_expected[] = { 1u, 2u, 3u, 4u, 5u, 6u };
    const uint32_t r2_words[] = { 0x01020304u, 0x05060708u, 0x090a0b0cu,
                                  0x0d0e0f10u, 0x00000011u };
    uint8_t r1[6];
    uint8_t r2[17];

    TEST_ASSERT_TRUE(pf_sdio_pio_response_words_to_bytes(r1_words, 48u, r1, sizeof(r1)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(r1_expected, r1, sizeof(r1));
    TEST_ASSERT_TRUE(pf_sdio_pio_response_words_to_bytes(r2_words, 136u, r2, sizeof(r2)));
    TEST_ASSERT_EQUAL_HEX8(0x11u, r2[16]);
    TEST_ASSERT_FALSE(pf_sdio_pio_response_words_to_bytes(r1_words, 48u, r1, 5u));
    TEST_ASSERT_FALSE(pf_sdio_pio_response_words_to_bytes(r1_words, 47u, r1, sizeof(r1)));
}

void test_decodes_native_cmd8_capture_without_bit_shift(void)
{
    const uint32_t words[] = { 0x08000001u, 0x0000aa13u };
    uint8_t response[6];
    uint32_t argument = 0u;

    TEST_ASSERT_TRUE(pf_sdio_pio_response_words_to_bytes(words, 48u, response,
                                                       sizeof(response)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0x08u, 0x00u, 0x00u, 0x01u, 0xaau, 0x13u}),
                                 response, sizeof(response));
    TEST_ASSERT_TRUE(pf_sdio_pio_parse_response(8u, response, &argument, true));
    TEST_ASSERT_EQUAL_HEX32(0x000001aau, argument);
}

void test_decodes_native_acmd41_capture_without_bit_shift(void)
{
    const uint32_t words[] = { 0x3fc0ff80u, 0x000000ffu };
    uint8_t response[6];
    uint32_t ocr = 0u;

    TEST_ASSERT_TRUE(pf_sdio_pio_response_words_to_bytes(words, 48u, response,
                                                       sizeof(response)));
    TEST_ASSERT_TRUE(pf_sdio_pio_parse_r3_response(response, &ocr));
    TEST_ASSERT_EQUAL_HEX32(0xc0ff8000u, ocr);
}

void test_classifies_cmd8_v2_and_legacy_cards(void)
{
    bool supports_v2;

    TEST_ASSERT_TRUE(pf_sdio_pio_cmd8_supports_v2(0x000001aau, &supports_v2));
    TEST_ASSERT_TRUE(supports_v2);
    TEST_ASSERT_TRUE(pf_sdio_pio_cmd8_supports_v2(1u << 22u, &supports_v2));
    TEST_ASSERT_FALSE(supports_v2);
    TEST_ASSERT_FALSE(pf_sdio_pio_cmd8_supports_v2(0x000001abu, &supports_v2));
    TEST_ASSERT_FALSE(pf_sdio_pio_cmd8_supports_v2(1u << 22u, NULL));
}

void test_decodes_data_write_response_from_low_fifo_nibble(void)
{
    TEST_ASSERT_TRUE(pf_sdio_pio_write_response_accepted(0x00000005u));
    TEST_ASSERT_FALSE(pf_sdio_pio_write_response_accepted(0x50000000u));
    TEST_ASSERT_FALSE(pf_sdio_pio_write_response_accepted(0x0000000bu));
}

void test_rejects_malformed_short_response_frames(void)
{
    uint8_t response[6] = { 0x11u, 0u, 0u, 0u, 0u, 0x01u };
    uint32_t argument;

    TEST_ASSERT_FALSE(pf_sdio_pio_parse_response(17u, NULL, &argument, false));
    TEST_ASSERT_FALSE(pf_sdio_pio_parse_response(17u, response, NULL, false));
    response[0] |= 0x40u;
    TEST_ASSERT_FALSE(pf_sdio_pio_parse_response(17u, response, &argument, false));
    response[0] &= 0xbfu;
    response[5] = 0u;
    TEST_ASSERT_FALSE(pf_sdio_pio_parse_response(17u, response, &argument, false));
}

void test_validates_r2_response_crc_and_end_bit(void)
{
    uint8_t response[17] = { 0u };

    response[0] = 0x3fu;
    for (size_t index = 1u; index < 16u; index++) {
        response[index] = (uint8_t)(index * 17u);
    }
    response[16] = (uint8_t)((pf_sdio_pio_crc7(response + 1u, 15u) << 1u) | 1u);
    TEST_ASSERT_TRUE(pf_sdio_pio_validate_r2_response(response));
    response[4] ^= 1u;
    TEST_ASSERT_FALSE(pf_sdio_pio_validate_r2_response(response));
    response[4] ^= 1u;
    response[16] &= 0xfeu;
    TEST_ASSERT_FALSE(pf_sdio_pio_validate_r2_response(response));
    TEST_ASSERT_FALSE(pf_sdio_pio_validate_r2_response(NULL));
}

void test_decodes_v2_csd_sector_count(void)
{
    uint8_t csd[16] = { 0u };
    uint32_t sectors;

    csd[0] = 0x40u;
    csd[7] = 0x03u;
    csd[8] = 0xffu;
    csd[9] = 0xffu;
    TEST_ASSERT_TRUE(pf_sdio_pio_csd_sector_count(csd, &sectors));
    TEST_ASSERT_EQUAL_UINT32(0x10000000u, sectors);
}

void test_decodes_v1_csd_sector_count(void)
{
    uint8_t csd[16] = { 0u };
    uint32_t sectors;

    csd[5] = 9u;
    csd[7] = 0xffu;
    csd[8] = 0xc0u;
    TEST_ASSERT_TRUE(pf_sdio_pio_csd_sector_count(csd, &sectors));
    TEST_ASSERT_EQUAL_UINT32(4096u, sectors);
}

void test_rejects_invalid_csd_inputs_and_structure(void)
{
    uint8_t csd[16] = { 0u };
    uint32_t sectors;

    TEST_ASSERT_FALSE(pf_sdio_pio_csd_sector_count(NULL, &sectors));
    TEST_ASSERT_FALSE(pf_sdio_pio_csd_sector_count(csd, NULL));
    csd[0] = 0x80u;
    TEST_ASSERT_FALSE(pf_sdio_pio_csd_sector_count(csd, &sectors));
    csd[0] = 0xc0u;
    TEST_ASSERT_FALSE(pf_sdio_pio_csd_sector_count(csd, &sectors));
}

void test_rejects_invalid_v1_block_length_and_zero_capacity(void)
{
    uint8_t csd[16] = { 0u };
    uint32_t sectors = 17u;

    csd[5] = 12u;
    TEST_ASSERT_FALSE(pf_sdio_pio_csd_sector_count(csd, &sectors));
    csd[5] = 0u;
    TEST_ASSERT_FALSE(pf_sdio_pio_csd_sector_count(csd, &sectors));
    TEST_ASSERT_EQUAL_UINT32(17u, sectors);
}

static const uint32_t identity_gpio[4] = { 4u, 5u, 6u, 7u };
static const uint32_t documented_gpio[4] = { 4u, 7u, 6u, 5u };

static void fill_pattern(uint8_t block[512])
{
    for (size_t index = 0u; index < 512u; index++) {
        block[index] = (uint8_t)(index * 37u + 11u);
    }
}

void test_round_trips_zero_filled_data_block_and_lane_crcs(void)
{
    pf_sdio_pio_lane_map_t map;
    uint8_t source[512] = { 0u };
    uint8_t destination[512];
    uint8_t image[PF_SDIO_PIO_IMAGE_SIZE];

    TEST_ASSERT_TRUE(pf_sdio_pio_lane_map_init(&map, identity_gpio));
    TEST_ASSERT_TRUE(pf_sdio_pio_encode_data_block(&map, source, image));
    TEST_ASSERT_TRUE(pf_sdio_pio_decode_data_block(&map, image, destination));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(source, destination, sizeof(source));
}

void test_round_trips_patterned_data_block_for_each_lane_order(void)
{
    const uint32_t *orders[] = { identity_gpio, documented_gpio };
    uint8_t source[512];
    uint8_t destination[512];
    uint8_t image[PF_SDIO_PIO_IMAGE_SIZE];

    fill_pattern(source);
    for (size_t order = 0u; order < 2u; order++) {
        pf_sdio_pio_lane_map_t map;

        TEST_ASSERT_TRUE(pf_sdio_pio_lane_map_init(&map, orders[order]));
        TEST_ASSERT_TRUE(pf_sdio_pio_encode_data_block(&map, source, image));
        memset(destination, 0, sizeof(destination));
        TEST_ASSERT_TRUE(pf_sdio_pio_decode_data_block(&map, image, destination));
        TEST_ASSERT_EQUAL_UINT8_ARRAY(source, destination, sizeof(source));
    }
}

void test_encodes_data_nibbles_and_crc_in_wire_order(void)
{
    pf_sdio_pio_lane_map_t map;
    uint8_t source[512];
    uint8_t image[PF_SDIO_PIO_IMAGE_SIZE];
    uint16_t crc[4];

    fill_pattern(source);
    TEST_ASSERT_TRUE(pf_sdio_pio_lane_map_init(&map, identity_gpio));
    TEST_ASSERT_TRUE(pf_sdio_pio_encode_data_block(&map, source, image));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(source, image, sizeof(source));

    /* CRC nibble n carries bit (15 - n) of each lane, most significant nibble first. */
    pf_sdio_pio_crc16_4bit(source, crc);
    for (size_t cycle = 0u; cycle < 16u; cycle++) {
        uint8_t nibble = 0u;
        uint8_t actual = image[512u + cycle / 2u];

        for (uint8_t lane = 0u; lane < 4u; lane++) {
            nibble |= (uint8_t)(((crc[lane] >> (15u - cycle)) & 1u) << lane);
        }
        TEST_ASSERT_EQUAL_HEX8(nibble, (cycle % 2u == 0u) ? (actual >> 4u) : (actual & 0x0fu));
    }
}

void test_maps_documented_dat_pin_order_and_back(void)
{
    pf_sdio_pio_lane_map_t map;
    uint8_t source[512] = { 0x12u, 0x34u, 0x56u, 0x78u };
    uint8_t image[PF_SDIO_PIO_IMAGE_SIZE];

    TEST_ASSERT_TRUE(pf_sdio_pio_lane_map_init(&map, documented_gpio));
    TEST_ASSERT_TRUE(pf_sdio_pio_encode_data_block(&map, source, image));
    TEST_ASSERT_EQUAL_HEX8(0x18u, image[0]);
    TEST_ASSERT_EQUAL_HEX8(0x94u, image[1]);
    TEST_ASSERT_EQUAL_HEX8(0x5cu, image[2]);
    TEST_ASSERT_EQUAL_HEX8(0xd2u, image[3]);
    for (uint8_t nibble = 0u; nibble < 16u; nibble++) {
        TEST_ASSERT_EQUAL_HEX8(nibble, map.from_gpio[map.to_gpio[nibble]]);
    }
}

void test_rejects_invalid_data_lane_gpio_mapping(void)
{
    pf_sdio_pio_lane_map_t map;
    const uint32_t duplicate_gpio[4] = { 4u, 5u, 5u, 7u };
    const uint32_t sparse_gpio[4] = { 4u, 6u, 7u, 8u };

    TEST_ASSERT_FALSE(pf_sdio_pio_lane_map_init(&map, duplicate_gpio));
    TEST_ASSERT_FALSE(pf_sdio_pio_lane_map_init(&map, sparse_gpio));
    TEST_ASSERT_FALSE(pf_sdio_pio_lane_map_init(NULL, identity_gpio));
    TEST_ASSERT_FALSE(pf_sdio_pio_lane_map_init(&map, NULL));
}

void test_calculates_known_four_lane_crc16_vector(void)
{
    uint8_t block[512];
    uint16_t crc[4];
    const uint16_t expected[4] = { 0x6aa3u, 0xa97du, 0x10b5u, 0x7357u };

    for (size_t index = 0u; index < sizeof(block); index++) {
        block[index] = (uint8_t)index;
    }
    pf_sdio_pio_crc16_4bit(block, crc);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(expected, crc, 4u);
}

void test_four_lane_crc_matches_independent_bitwise_reference(void)
{
    uint8_t block[512];
    uint16_t actual[4];
    uint32_t pattern = 0x953a7e21u;

    for (uint32_t sample = 0u; sample < 8u; sample++) {
        uint16_t expected[4] = { 0u };

        for (size_t index = 0u; index < sizeof(block); index++) {
            pattern = pattern * 1664525u + 1013904223u;
            block[index] = (uint8_t)(pattern >> 24u);
            for (uint32_t half = 0u; half < 2u; half++) {
                uint8_t nibble = (uint8_t)(block[index] >> (half == 0u ? 4u : 0u));
                for (uint32_t lane = 0u; lane < 4u; lane++) {
                    uint16_t feedback = (uint16_t)((expected[lane] >> 15u) ^
                                                  ((nibble >> lane) & 1u));
                    expected[lane] = (uint16_t)(expected[lane] << 1u);
                    if (feedback != 0u) {
                        expected[lane] ^= 0x1021u;
                    }
                }
            }
        }
        pf_sdio_pio_crc16_4bit(block, actual);
        TEST_ASSERT_EQUAL_UINT16_ARRAY(expected, actual, 4u);
    }
}

void test_detects_corrupt_data_and_crc_bytes(void)
{
    pf_sdio_pio_lane_map_t map;
    uint8_t source[512] = { 0u };
    uint8_t destination[512];
    uint8_t image[PF_SDIO_PIO_IMAGE_SIZE];

    TEST_ASSERT_TRUE(pf_sdio_pio_lane_map_init(&map, documented_gpio));
    TEST_ASSERT_TRUE(pf_sdio_pio_encode_data_block(&map, source, image));
    image[0] ^= 0x10u;
    TEST_ASSERT_FALSE(pf_sdio_pio_decode_data_block(&map, image, destination));
    image[0] ^= 0x10u;
    image[512] ^= 1u;
    TEST_ASSERT_FALSE(pf_sdio_pio_decode_data_block(&map, image, destination));
    image[512] ^= 1u;
    image[PF_SDIO_PIO_IMAGE_SIZE - 1u] ^= 0x80u;
    TEST_ASSERT_FALSE(pf_sdio_pio_decode_data_block(&map, image, destination));
}

void test_handles_null_data_block_buffers(void)
{
    pf_sdio_pio_lane_map_t map;
    uint16_t crc[4];
    uint8_t image[PF_SDIO_PIO_IMAGE_SIZE] = { 0u };
    uint8_t block[512];

    TEST_ASSERT_TRUE(pf_sdio_pio_lane_map_init(&map, identity_gpio));
    pf_sdio_pio_crc16_4bit(NULL, crc);
    pf_sdio_pio_crc16_4bit(block, NULL);
    TEST_ASSERT_FALSE(pf_sdio_pio_encode_data_block(NULL, block, image));
    TEST_ASSERT_FALSE(pf_sdio_pio_encode_data_block(&map, NULL, image));
    TEST_ASSERT_FALSE(pf_sdio_pio_encode_data_block(&map, block, NULL));
    TEST_ASSERT_FALSE(pf_sdio_pio_decode_data_block(NULL, image, block));
    TEST_ASSERT_FALSE(pf_sdio_pio_decode_data_block(&map, NULL, block));
    TEST_ASSERT_FALSE(pf_sdio_pio_decode_data_block(&map, image, NULL));
}
static void reference_crc16_lanes(const uint8_t block[512], uint16_t crc[4])
{
    for (uint32_t lane = 0u; lane < 4u; lane++) {
        crc[lane] = 0u;
    }
    for (size_t index = 0u; index < 512u; index++) {
        for (uint32_t half = 0u; half < 2u; half++) {
            uint8_t nibble = (uint8_t)(block[index] >> (half == 0u ? 4u : 0u));
            for (uint32_t lane = 0u; lane < 4u; lane++) {
                uint16_t feedback = (uint16_t)((crc[lane] >> 15u) ^ ((nibble >> lane) & 1u));
                crc[lane] = (uint16_t)(crc[lane] << 1u);
                if (feedback != 0u) {
                    crc[lane] ^= 0x1021u;
                }
            }
        }
    }
}

/* Builds the wire image from the bitwise reference, independent of the production CRC. */
static void reference_image(const uint8_t block[512], const uint32_t gpio[4],
                            uint8_t image[PF_SDIO_PIO_IMAGE_SIZE])
{
    uint16_t crc[4];
    uint32_t base = gpio[0];

    for (uint32_t lane = 1u; lane < 4u; lane++) {
        if (gpio[lane] < base) {
            base = gpio[lane];
        }
    }
    reference_crc16_lanes(block, crc);
    for (size_t index = 0u; index < 512u; index++) {
        uint8_t byte = 0u;
        for (uint32_t half = 0u; half < 2u; half++) {
            uint8_t nibble = (uint8_t)(block[index] >> (half == 0u ? 4u : 0u));
            uint8_t wire = 0u;
            for (uint32_t lane = 0u; lane < 4u; lane++) {
                wire |= (uint8_t)(((nibble >> lane) & 1u) << (gpio[lane] - base));
            }
            byte |= (uint8_t)(wire << (half == 0u ? 4u : 0u));
        }
        image[index] = byte;
    }
    for (size_t index = 0u; index < 8u; index++) {
        uint8_t byte = 0u;
        for (uint32_t half = 0u; half < 2u; half++) {
            uint32_t bit = 15u - (2u * (uint32_t)index + half);
            uint8_t wire = 0u;
            for (uint32_t lane = 0u; lane < 4u; lane++) {
                wire |= (uint8_t)(((crc[lane] >> bit) & 1u) << (gpio[lane] - base));
            }
            byte |= (uint8_t)(wire << (half == 0u ? 4u : 0u));
        }
        image[512u + index] = byte;
    }
}

void test_data_block_crc_matches_reference_for_every_lane_permutation(void)
{
    uint8_t source[512];
    uint8_t destination[512];
    uint8_t image[PF_SDIO_PIO_IMAGE_SIZE];
    uint8_t expected[PF_SDIO_PIO_IMAGE_SIZE];
    uint32_t pattern = 0x1234567u;

    for (uint32_t code = 0u; code < 256u; code++) {
        uint32_t gpio[4] = { 4u + (code & 3u), 4u + ((code >> 2u) & 3u),
                             4u + ((code >> 4u) & 3u), 4u + ((code >> 6u) & 3u) };
        pf_sdio_pio_lane_map_t map;
        bool valid = true;

        for (uint32_t a = 0u; a < 4u; a++) {
            for (uint32_t b = a + 1u; b < 4u; b++) {
                valid = valid && (gpio[a] != gpio[b]);
            }
        }
        if (!valid) {
            continue;
        }
        for (size_t index = 0u; index < sizeof(source); index++) {
            pattern = pattern * 1664525u + 1013904223u;
            source[index] = (uint8_t)(pattern >> 24u);
        }
        reference_image(source, gpio, expected);
        TEST_ASSERT_TRUE(pf_sdio_pio_lane_map_init(&map, gpio));
        TEST_ASSERT_TRUE(pf_sdio_pio_encode_data_block(&map, source, image));
        TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, image, sizeof(image));
        TEST_ASSERT_TRUE(pf_sdio_pio_decode_data_block(&map, expected, destination));
        TEST_ASSERT_EQUAL_UINT8_ARRAY(source, destination, sizeof(source));
    }
}

void test_decode_rejects_every_single_bit_crc_error_per_lane(void)
{
    pf_sdio_pio_lane_map_t map;
    uint8_t source[512];
    uint8_t destination[512];
    uint8_t image[PF_SDIO_PIO_IMAGE_SIZE];

    fill_pattern(source);
    TEST_ASSERT_TRUE(pf_sdio_pio_lane_map_init(&map, documented_gpio));
    TEST_ASSERT_TRUE(pf_sdio_pio_encode_data_block(&map, source, image));
    for (size_t index = 512u; index < PF_SDIO_PIO_IMAGE_SIZE; index++) {
        for (uint32_t bit = 0u; bit < 8u; bit++) {
            image[index] ^= (uint8_t)(1u << bit);
            TEST_ASSERT_FALSE(pf_sdio_pio_decode_data_block(&map, image, destination));
            image[index] ^= (uint8_t)(1u << bit);
        }
    }
    for (size_t index = 0u; index < 512u; index += 61u) {
        for (uint32_t bit = 0u; bit < 8u; bit++) {
            image[index] ^= (uint8_t)(1u << bit);
            TEST_ASSERT_FALSE(pf_sdio_pio_decode_data_block(&map, image, destination));
            image[index] ^= (uint8_t)(1u << bit);
        }
    }
    TEST_ASSERT_TRUE(pf_sdio_pio_decode_data_block(&map, image, destination));
}


void test_decode_byte_table_matches_nibble_map_for_all_byte_values(void)
{
    pf_sdio_pio_lane_map_t map;
    uint8_t source[512];
    uint8_t destination[512];
    uint8_t image[PF_SDIO_PIO_IMAGE_SIZE];
    const uint32_t gpio[4] = { 7u, 5u, 4u, 6u };

    TEST_ASSERT_TRUE(pf_sdio_pio_lane_map_init(&map, gpio));
    for (uint32_t value = 0u; value < 256u; value++) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)((map.from_gpio[value >> 4u] << 4u) |
                                          map.from_gpio[value & 0x0fu]),
                                map.from_gpio_byte[value]);
        source[value] = (uint8_t)value;
        source[256u + value] = (uint8_t)(255u - value);
    }
    TEST_ASSERT_TRUE(pf_sdio_pio_encode_data_block(&map, source, image));
    TEST_ASSERT_TRUE(pf_sdio_pio_decode_data_block(&map, image, destination));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(source, destination, sizeof(source));
}
