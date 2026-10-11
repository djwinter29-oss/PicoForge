#include "unity.h"
#include "sd_card_protocol.h"

void setUp(void) {}
void tearDown(void) {}

void test_shared_transfer_bounds_and_zero_count(void)
{
    uint8_t buffer[512];

    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sd_card_validate_transfer(0u, NULL, 0u, 0u));
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sd_card_validate_transfer(10u, buffer, 300u, 310u));
    TEST_ASSERT_EQUAL(PF_SD_CARD_INVALID_ARGUMENT, pf_sd_card_validate_transfer(10u, buffer, 301u, 310u));
    TEST_ASSERT_EQUAL(PF_SD_CARD_INVALID_ARGUMENT, pf_sd_card_validate_transfer(0u, NULL, 1u, 1u));
    TEST_ASSERT_EQUAL(PF_SD_CARD_INVALID_ARGUMENT,
                      pf_sd_card_validate_transfer(UINT32_MAX, buffer, 1u, UINT32_MAX));
}

void test_shared_serial_crc16_reference(void)
{
    const uint8_t bytes[] = "123456789";

    TEST_ASSERT_EQUAL_HEX16(0x31c3u, pf_sd_card_crc16(bytes, 9u));
    TEST_ASSERT_EQUAL_HEX16(0u, pf_sd_card_crc16(NULL, 9u));
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
    TEST_ASSERT_EQUAL_HEX16(expected, pf_sd_card_crc16(block, sizeof(block)));
}

static uint32_t idle_calls;

static void count_idle(void)
{
    idle_calls++;
}

void test_idle_hook_runs_only_when_set(void)
{
    idle_calls = 0u;
    pf_sd_card_idle();
    TEST_ASSERT_EQUAL_UINT32(0u, idle_calls);
    pf_sd_card_set_idle_hook(count_idle);
    pf_sd_card_idle();
    TEST_ASSERT_EQUAL_UINT32(1u, idle_calls);
    pf_sd_card_set_idle_hook(NULL);
    pf_sd_card_idle();
    TEST_ASSERT_EQUAL_UINT32(1u, idle_calls);
}

void test_shared_command_reference(void)
{
    uint8_t frame[6];
    const uint8_t expected[] = {0x40u, 0u, 0u, 0u, 0u, 0x95u};

    TEST_ASSERT_TRUE(pf_sd_card_build_command(0u, 0u, frame));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame, 6u);
    TEST_ASSERT_FALSE(pf_sd_card_build_command(64u, 0u, frame));
    TEST_ASSERT_FALSE(pf_sd_card_build_command(0u, 0u, NULL));
}