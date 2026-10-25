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
    TEST_ASSERT_EQUAL_UINT32(PICO_DEFAULT_UART, profile->monitor_uart_index);
    TEST_ASSERT_EQUAL_UINT32(PICO_DEFAULT_UART_TX_PIN, profile->monitor_uart_tx_gpio);
    TEST_ASSERT_EQUAL_UINT32(PICO_DEFAULT_UART_RX_PIN, profile->monitor_uart_rx_gpio);
#ifdef PICO_DEFAULT_LED_PIN
    TEST_ASSERT_EQUAL_UINT32(PICO_DEFAULT_LED_PIN, profile->led_gpio);
    TEST_ASSERT_EQUAL(PICO_DEFAULT_LED_PIN_INVERTED == 0, profile->led_active_high);
#else
    TEST_ASSERT_EQUAL_UINT32(PF_BOARD_PIN_UNASSIGNED, profile->led_gpio);
#endif
    TEST_ASSERT_EQUAL_UINT32(25000000u, profile->sd_clock_hz);
    TEST_ASSERT_EQUAL_UINT32(0u, profile->pio_index);
    TEST_ASSERT_EQUAL_UINT32(2u, profile->sdio_pio_pins.clock_gpio);
    TEST_ASSERT_EQUAL_UINT32(3u, profile->sdio_pio_pins.command_gpio);
    for (unsigned lane = 0; lane < 4; ++lane) {
        TEST_ASSERT_EQUAL_UINT32(4u + lane, profile->sdio_pio_pins.data_gpio[lane]);
    }
}
