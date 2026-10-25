#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/sdio")

#define PF_TEST_BOARD_CONFIG
#include "sdio_board_fixture.h"

void setUp(void) {}
void tearDown(void) {}

void test_standard_board_config_requests_qualified_sdio_clock_and_consecutive_lanes(void)
{
    TEST_ASSERT_EQUAL_UINT32(PICO_DEFAULT_UART, pf_monitor_uart_board.uart_index);
    TEST_ASSERT_EQUAL_UINT32(PICO_DEFAULT_UART_TX_PIN, pf_monitor_uart_board.tx_gpio);
    TEST_ASSERT_EQUAL_UINT32(PICO_DEFAULT_UART_RX_PIN, pf_monitor_uart_board.rx_gpio);
    TEST_ASSERT_EQUAL_UINT32(115200u, pf_monitor_uart_board.baud_rate);
#ifdef PICO_DEFAULT_LED_PIN
    TEST_ASSERT_EQUAL_UINT32(PICO_DEFAULT_LED_PIN, pf_led_board.gpio);
    TEST_ASSERT_EQUAL(PICO_DEFAULT_LED_PIN_INVERTED == 0, pf_led_board.active_high);
#else
    TEST_ASSERT_EQUAL_UINT32(PF_LED_GPIO_NONE, pf_led_board.gpio);
#endif
    TEST_ASSERT_EQUAL_UINT32(25000000u, pf_sd_board.clock_hz);
    TEST_ASSERT_EQUAL_UINT32(0u, pf_sdio_pio_board.pio_index);
    TEST_ASSERT_EQUAL_UINT32(2u, pf_sdio_pio_board.pins.clock_gpio);
    TEST_ASSERT_EQUAL_UINT32(3u, pf_sdio_pio_board.pins.command_gpio);
    for (unsigned lane = 0; lane < 4; ++lane) {
        TEST_ASSERT_EQUAL_UINT32(4u + lane, pf_sdio_pio_board.pins.data_gpio[lane]);
    }
}
