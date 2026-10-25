#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/sdio")

#define PF_CONFIG_SD_CARD_TRANSPORT 1
#include "board/board_profile.c"

void setUp(void) {}
void tearDown(void) {}

void test_spi_board_profile_contains_only_selected_transport_wiring(void)
{
    const pf_board_resources_t *profile = pf_board_resources_get();
    TEST_ASSERT_NOT_NULL(profile);
    TEST_ASSERT_EQUAL_UINT32(50000000u, profile->sd_clock_hz);
    TEST_ASSERT_EQUAL_UINT32(0u, profile->spi_index);
    TEST_ASSERT_EQUAL_UINT32(2u, profile->spi_pins.clock_gpio);
    TEST_ASSERT_EQUAL_UINT32(3u, profile->spi_pins.mosi_gpio);
    TEST_ASSERT_EQUAL_UINT32(4u, profile->spi_pins.miso_gpio);
    TEST_ASSERT_EQUAL_UINT32(7u, profile->spi_pins.cs_gpio);
}
