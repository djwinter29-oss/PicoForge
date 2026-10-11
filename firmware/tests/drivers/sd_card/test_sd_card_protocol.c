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

void test_shared_command_reference(void)
{
    uint8_t frame[6];
    const uint8_t expected[] = {0x40u, 0u, 0u, 0u, 0u, 0x95u};

    TEST_ASSERT_TRUE(pf_sd_card_build_command(0u, 0u, frame));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame, 6u);
    TEST_ASSERT_FALSE(pf_sd_card_build_command(64u, 0u, frame));
    TEST_ASSERT_FALSE(pf_sd_card_build_command(0u, 0u, NULL));
}