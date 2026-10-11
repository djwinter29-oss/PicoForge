#include "unity.h"
#include "sd_spi_protocol.h"
#include "sd_card_block.h"
#include "sd_card_protocol.h"

void setUp(void) {}
void tearDown(void) {}

void test_accepts_only_ready_or_idle_r1_responses(void)
{
    TEST_ASSERT_TRUE(pf_sd_spi_r1_is_ready_or_idle(0u));
    TEST_ASSERT_TRUE(pf_sd_spi_r1_is_ready_or_idle(1u));
    TEST_ASSERT_FALSE(pf_sd_spi_r1_is_ready_or_idle(2u));
    TEST_ASSERT_FALSE(pf_sd_spi_r1_is_ready_or_idle(0xffu));
}

void test_recognizes_start_block_token(void)
{
    TEST_ASSERT_TRUE(pf_sd_spi_read_token_is_valid(0xfeu));
    TEST_ASSERT_FALSE(pf_sd_spi_read_token_is_valid(0xffu));
    TEST_ASSERT_FALSE(pf_sd_spi_read_token_is_valid(0xfdu));
}

void test_recognizes_accepted_write_response(void)
{
    TEST_ASSERT_TRUE(pf_sd_spi_write_response_is_accepted(0x05u));
    TEST_ASSERT_TRUE(pf_sd_spi_write_response_is_accepted(0xe5u));
    TEST_ASSERT_FALSE(pf_sd_spi_write_response_is_accepted(0x0bu));
}

void test_converts_block_and_byte_address_arguments(void)
{
    uint32_t argument = 0u;

    TEST_ASSERT_TRUE(pf_sd_spi_block_argument(0x12345678u, true, &argument));
    TEST_ASSERT_EQUAL_HEX32(0x12345678u, argument);
    TEST_ASSERT_TRUE(pf_sd_spi_block_argument(17u, false, &argument));
    TEST_ASSERT_EQUAL_UINT32(17u * PF_SD_CARD_BLOCK_SIZE, argument);
}

void test_rejects_unrepresentable_byte_address(void)
{
    uint32_t argument = 0u;

    TEST_ASSERT_FALSE(pf_sd_spi_block_argument(
        (UINT32_MAX / PF_SD_CARD_BLOCK_SIZE) + 1u, false, &argument));
    TEST_ASSERT_FALSE(pf_sd_spi_block_argument(0u, true, NULL));
}

void test_matches_spi_pins_to_controller_roles(void)
{
    TEST_ASSERT_TRUE(pf_sd_spi_pins_match_controller(0u, 2u, 3u, 4u));
    TEST_ASSERT_TRUE(pf_sd_spi_pins_match_controller(0u, 6u, 7u, 0u));
    TEST_ASSERT_TRUE(pf_sd_spi_pins_match_controller(1u, 10u, 11u, 8u));
    TEST_ASSERT_TRUE(pf_sd_spi_pins_match_controller(0u, 34u, 35u, 36u));
    TEST_ASSERT_FALSE(pf_sd_spi_pins_match_controller(1u, 2u, 3u, 4u));
    TEST_ASSERT_FALSE(pf_sd_spi_pins_match_controller(0u, 2u, 3u, 5u));
    TEST_ASSERT_FALSE(pf_sd_spi_pins_match_controller(0u, 2u, 11u, 4u));
    TEST_ASSERT_FALSE(pf_sd_spi_pins_match_controller(2u, 2u, 3u, 4u));
}