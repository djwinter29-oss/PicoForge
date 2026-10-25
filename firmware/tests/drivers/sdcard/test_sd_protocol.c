#include "unity.h"
#include "sd_protocol.h"

void setUp(void) {}
void tearDown(void) {}

void test_converts_block_and_byte_address_arguments(void)
{
    uint32_t argument = 0u;

    TEST_ASSERT_TRUE(pf_sd_block_argument(0x12345678u, true, &argument));
    TEST_ASSERT_EQUAL_HEX32(0x12345678u, argument);
    TEST_ASSERT_TRUE(pf_sd_block_argument(17u, false, &argument));
    TEST_ASSERT_EQUAL_UINT32(17u * PF_SD_BLOCK_SIZE, argument);
    TEST_ASSERT_FALSE(pf_sd_block_argument((UINT32_MAX / PF_SD_BLOCK_SIZE) + 1u, false, &argument));
    TEST_ASSERT_FALSE(pf_sd_block_argument(0u, true, NULL));
}

void test_shared_transfer_bounds_and_zero_count(void)
{
    uint8_t buffer[512];

    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_validate_transfer(0u, NULL, 0u, 0u));
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_validate_transfer(10u, buffer, 300u, 310u));
    TEST_ASSERT_EQUAL(PF_SD_INVALID_ARGUMENT, pf_sd_validate_transfer(10u, buffer, 301u, 310u));
    TEST_ASSERT_EQUAL(PF_SD_INVALID_ARGUMENT, pf_sd_validate_transfer(0u, NULL, 1u, 1u));
    TEST_ASSERT_EQUAL(PF_SD_INVALID_ARGUMENT,
                      pf_sd_validate_transfer(UINT32_MAX, buffer, 1u, UINT32_MAX));
}

void test_shared_transfer_byte_count_must_fit_size_t(void)
{
    uint8_t buffer[1];
    uint32_t largest_transfer = (SIZE_MAX / PF_SD_BLOCK_SIZE < UINT32_MAX)
        ? (uint32_t)(SIZE_MAX / PF_SD_BLOCK_SIZE) : UINT32_MAX;

    TEST_ASSERT_EQUAL(PF_SD_OK,
                      pf_sd_validate_transfer(0u, buffer, largest_transfer, UINT32_MAX));
    if (largest_transfer < UINT32_MAX) {
        TEST_ASSERT_EQUAL(PF_SD_INVALID_ARGUMENT,
                          pf_sd_validate_transfer(0u, buffer, largest_transfer + 1u,
                                                       UINT32_MAX));
    }
}

void test_shared_serial_crc16_reference(void)
{
    const uint8_t bytes[] = "123456789";

    TEST_ASSERT_EQUAL_HEX16(0x31c3u, pf_sd_crc16(bytes, 9u));
    TEST_ASSERT_EQUAL_HEX16(0u, pf_sd_crc16(NULL, 9u));
}

void test_shared_crc16_matches_bitwise_reference_for_a_sector(void)
{
    uint8_t block[512];
    uint16_t expected = 0u;

    for (size_t index = 0u; index < sizeof(block); index++) {
        block[index] = (uint8_t)(index * 73u + 5u);
        expected ^= (uint16_t)block[index] << 8u;
        for (int bit = 0; bit < 8; bit++) {
            expected = (expected & 0x8000u) ? (uint16_t)((expected << 1u) ^ 0x1021u)
                                            : (uint16_t)(expected << 1u);
        }
    }
    TEST_ASSERT_EQUAL_HEX16(expected, pf_sd_crc16(block, sizeof(block)));
}

static uint32_t idle_calls;

static void count_idle(void)
{
    idle_calls++;
}

void test_idle_hook_runs_only_when_set(void)
{
    idle_calls = 0u;
    pf_sd_idle();
    TEST_ASSERT_EQUAL_UINT32(0u, idle_calls);
    pf_sd_set_idle_hook(count_idle);
    pf_sd_idle();
    TEST_ASSERT_EQUAL_UINT32(1u, idle_calls);
    pf_sd_set_idle_hook(NULL);
    pf_sd_idle();
    TEST_ASSERT_EQUAL_UINT32(1u, idle_calls);
}

void test_shared_command_reference(void)
{
    uint8_t frame[6];
    const uint8_t expected[] = {0x40u, 0u, 0u, 0u, 0u, 0x95u};

    TEST_ASSERT_TRUE(pf_sd_build_command(0u, 0u, frame));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame, 6u);
    TEST_ASSERT_FALSE(pf_sd_build_command(64u, 0u, frame));
    TEST_ASSERT_FALSE(pf_sd_build_command(0u, 0u, NULL));
}

void test_csd_transfer_speed_decodes_standard_units_and_multipliers(void)
{
    uint8_t csd[16] = { 0u };
    uint32_t transfer_hz = 0u;

    csd[3] = 0x32u;
    TEST_ASSERT_TRUE(pf_sd_csd_transfer_hz(csd, &transfer_hz));
    TEST_ASSERT_EQUAL_UINT32(25000000u, transfer_hz);
    csd[3] = 0x5au;
    TEST_ASSERT_TRUE(pf_sd_csd_transfer_hz(csd, &transfer_hz));
    TEST_ASSERT_EQUAL_UINT32(50000000u, transfer_hz);
    csd[3] = 0x7bu;
    TEST_ASSERT_TRUE(pf_sd_csd_transfer_hz(csd, &transfer_hz));
    TEST_ASSERT_EQUAL_UINT32(800000000u, transfer_hz);
    csd[3] = 0xb2u;
    TEST_ASSERT_FALSE(pf_sd_csd_transfer_hz(csd, &transfer_hz));
    csd[3] = 0x0fu;
    TEST_ASSERT_FALSE(pf_sd_csd_transfer_hz(csd, &transfer_hz));
    TEST_ASSERT_FALSE(pf_sd_csd_transfer_hz(NULL, &transfer_hz));
    TEST_ASSERT_FALSE(pf_sd_csd_transfer_hz(csd, NULL));
}

void test_scr_reports_four_bit_bus_support(void)
{
    uint8_t scr[8] = { 0u };

    TEST_ASSERT_FALSE(pf_sd_scr_supports_four_bit(scr));
    scr[1] = 0x04u;
    TEST_ASSERT_TRUE(pf_sd_scr_supports_four_bit(scr));
    TEST_ASSERT_FALSE(pf_sd_scr_supports_four_bit(NULL));
}
void test_crc7_matches_cmd0_reference(void)
{
    const uint8_t cmd0_header[] = { 0x40u, 0x00u, 0x00u, 0x00u, 0x00u };

    TEST_ASSERT_EQUAL_UINT8(0x4au, pf_sd_crc7(cmd0_header, sizeof(cmd0_header)));
}

void test_crc7_returns_zero_for_null_and_empty_input(void)
{
    const uint8_t bytes[] = { 0xffu };

    TEST_ASSERT_EQUAL_UINT8(0u, pf_sd_crc7(NULL, sizeof(bytes)));
    TEST_ASSERT_EQUAL_UINT8(0u, pf_sd_crc7(bytes, 0u));
}

void test_builds_cmd0_frame_with_standard_crc(void)
{
    uint8_t frame[6];
    const uint8_t expected[] = { 0x40u, 0x00u, 0x00u, 0x00u, 0x00u, 0x95u };

    TEST_ASSERT_TRUE(pf_sd_build_command(0u, 0u, frame));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame, sizeof(expected));
}

void test_builds_cmd8_frame_with_standard_crc(void)
{
    uint8_t frame[6];
    const uint8_t expected[] = { 0x48u, 0x00u, 0x00u, 0x01u, 0xaau, 0x87u };

    TEST_ASSERT_TRUE(pf_sd_build_command(8u, 0x000001aau, frame));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame, sizeof(expected));
}

void test_builds_max_command_and_big_endian_argument(void)
{
    uint8_t frame[6];

    TEST_ASSERT_TRUE(pf_sd_build_command(63u, 0x89abcdefu, frame));
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

    TEST_ASSERT_FALSE(pf_sd_build_command(64u, 0u, frame));
    TEST_ASSERT_FALSE(pf_sd_build_command(0u, 0u, NULL));
}

void test_classifies_cmd8_v2_and_legacy_cards(void)
{
    bool supports_v2;

    TEST_ASSERT_TRUE(pf_sd_cmd8_supports_v2(0x000001aau, &supports_v2));
    TEST_ASSERT_TRUE(supports_v2);
    TEST_ASSERT_TRUE(pf_sd_cmd8_supports_v2(1u << 22u, &supports_v2));
    TEST_ASSERT_FALSE(supports_v2);
    TEST_ASSERT_FALSE(pf_sd_cmd8_supports_v2(0x000001abu, &supports_v2));
    TEST_ASSERT_FALSE(pf_sd_cmd8_supports_v2(1u << 22u, NULL));
}

void test_decodes_v2_csd_sector_count(void)
{
    uint8_t csd[16] = { 0u };
    uint32_t sectors;

    csd[0] = 0x40u;
    csd[7] = 0x03u;
    csd[8] = 0xffu;
    csd[9] = 0xffu;
    TEST_ASSERT_TRUE(pf_sd_csd_sector_count(csd, &sectors));
    TEST_ASSERT_EQUAL_UINT32(0x10000000u, sectors);
}

void test_decodes_v1_csd_sector_count(void)
{
    uint8_t csd[16] = { 0u };
    uint32_t sectors;

    csd[5] = 9u;
    csd[7] = 0xffu;
    csd[8] = 0xc0u;
    TEST_ASSERT_TRUE(pf_sd_csd_sector_count(csd, &sectors));
    TEST_ASSERT_EQUAL_UINT32(4096u, sectors);
}

void test_rejects_invalid_csd_inputs_and_structure(void)
{
    uint8_t csd[16] = { 0u };
    uint32_t sectors;

    TEST_ASSERT_FALSE(pf_sd_csd_sector_count(NULL, &sectors));
    TEST_ASSERT_FALSE(pf_sd_csd_sector_count(csd, NULL));
    csd[0] = 0x80u;
    TEST_ASSERT_FALSE(pf_sd_csd_sector_count(csd, &sectors));
    csd[0] = 0xc0u;
    TEST_ASSERT_FALSE(pf_sd_csd_sector_count(csd, &sectors));
}

void test_rejects_invalid_v1_block_length_and_zero_capacity(void)
{
    uint8_t csd[16] = { 0u };
    uint32_t sectors = 17u;

    csd[5] = 12u;
    TEST_ASSERT_FALSE(pf_sd_csd_sector_count(csd, &sectors));
    csd[5] = 0u;
    TEST_ASSERT_FALSE(pf_sd_csd_sector_count(csd, &sectors));
    TEST_ASSERT_EQUAL_UINT32(17u, sectors);
}
