#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/sdio")

#define PF_TEST_BOARD_PROFILE
#include "sdio_board_profile_fixture.h"

void setUp(void) {}
void tearDown(void) {}

void test_standard_board_profile_requests_qualified_sdio_clock_and_consecutive_lanes(void)
{
    const pf_board_resources_t *profile = pf_board_resources_get();
    TEST_ASSERT_NOT_NULL(profile);
    TEST_ASSERT_TRUE(profile->sd_card_enabled);
    TEST_ASSERT_EQUAL_UINT32(25000000u, profile->clock_hz);
    TEST_ASSERT_EQUAL_UINT32(2u, profile->native_pins.clock_gpio);
    TEST_ASSERT_EQUAL_UINT32(3u, profile->native_pins.command_gpio);
    for (unsigned lane = 0; lane < 4; ++lane) {
        TEST_ASSERT_EQUAL_UINT32(4u + lane, profile->native_pins.data_gpio[lane]);
    }
}
