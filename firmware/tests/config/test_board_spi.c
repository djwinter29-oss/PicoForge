#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/sdio")

#define PF_CONFIG_SDCARD_TRANSPORT 1
#include "config/board.c"

void setUp(void) {}
void tearDown(void) {}

void test_spi_board_config_contains_only_selected_transport_wiring(void)
{
    TEST_ASSERT_EQUAL_UINT32(50000000u, pf_sd_board.clock_hz);
    TEST_ASSERT_EQUAL_UINT32(0u, pf_spi_board.spi_index);
    TEST_ASSERT_EQUAL_UINT32(2u, pf_spi_board.pins.clock_gpio);
    TEST_ASSERT_EQUAL_UINT32(3u, pf_spi_board.pins.mosi_gpio);
    TEST_ASSERT_EQUAL_UINT32(4u, pf_spi_board.pins.miso_gpio);
    TEST_ASSERT_EQUAL_UINT32(7u, pf_spi_board.pins.cs_gpio);
}
