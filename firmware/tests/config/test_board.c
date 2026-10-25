#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/sdio")

#define PF_TEST_BOARD_CONFIG
#include "sdio_board_fixture.h"

void setUp(void) {}
void tearDown(void) {}

void test_board_config_describes_uart_led_and_both_storage_transports(void)
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
    TEST_ASSERT_EQUAL_UINT32(8u, pf_sd_board.card_detect_gpio);
    TEST_ASSERT_TRUE(pf_sd_board.card_detect_active_high);
    TEST_ASSERT_EQUAL_UINT32(0u, pf_sdio_pio_board.pio_index);
    TEST_ASSERT_EQUAL_UINT32(2u, pf_sdio_pio_board.pins.clock_gpio);
    TEST_ASSERT_EQUAL_UINT32(3u, pf_sdio_pio_board.pins.command_gpio);
    for (unsigned lane = 0; lane < 4; ++lane) {
        TEST_ASSERT_EQUAL_UINT32(4u + lane, pf_sdio_pio_board.pins.data_gpio[lane]);
    }
    TEST_ASSERT_EQUAL_UINT32(0u, pf_spi_board.spi_index);
    TEST_ASSERT_EQUAL_UINT32(2u, pf_spi_board.pins.clock_gpio);
    TEST_ASSERT_EQUAL_UINT32(3u, pf_spi_board.pins.mosi_gpio);
    TEST_ASSERT_EQUAL_UINT32(4u, pf_spi_board.pins.miso_gpio);
    TEST_ASSERT_EQUAL_UINT32(7u, pf_spi_board.pins.cs_gpio);
}
