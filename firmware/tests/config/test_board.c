#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/sdio")

#define PF_TEST_BOARD_CONFIG
#define PF_BOARD_SD_SPI 0
#include "sdio_board_fixture.h"

#ifdef PF_SPI_PINS_H
#error "SDIO board configuration must not include SPI assignments"
#endif

void setUp(void) {}
void tearDown(void) {}

void test_board_gpio_assignments_are_bytes_and_none_is_255(void)
{
    TEST_ASSERT_EQUAL_UINT8(255u, PF_LED_GPIO_NONE);
    TEST_ASSERT_EQUAL_UINT(1u, sizeof(pf_led_board.gpio));
    TEST_ASSERT_EQUAL_UINT(1u, sizeof(pf_monitor_uart_board.uart_index));
    TEST_ASSERT_EQUAL_UINT(1u, sizeof(pf_monitor_uart_board.tx_gpio));
    TEST_ASSERT_EQUAL_UINT(1u, sizeof(pf_monitor_uart_board.rx_gpio));
    TEST_ASSERT_EQUAL_UINT(1u, sizeof(pf_sdio_pio_board_pins.card_detect_gpio));
    TEST_ASSERT_EQUAL_UINT(6u, sizeof(pf_sdio_pio_board_pins.pins));
    TEST_ASSERT_EQUAL_UINT(1u, sizeof(pf_sdio_pio_board_pins.pio_index));
    TEST_ASSERT_EQUAL_UINT(9u, sizeof(pf_sdio_pio_board_pins));
}

void test_board_config_describes_uart_led_and_sdio_socket_assignment(void)
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
    TEST_ASSERT_EQUAL_UINT8(8u, pf_sdio_pio_board_pins.card_detect_gpio);
    TEST_ASSERT_TRUE(pf_sdio_pio_board_pins.card_detect_active_high);
    TEST_ASSERT_EQUAL_UINT8(0u, pf_sdio_pio_board_pins.pio_index);
    TEST_ASSERT_EQUAL_UINT8(2u, pf_sdio_pio_board_pins.pins.clock_gpio);
    TEST_ASSERT_EQUAL_UINT8(3u, pf_sdio_pio_board_pins.pins.command_gpio);
    for (unsigned lane = 0; lane < 4; ++lane) {
        TEST_ASSERT_EQUAL_UINT8(4u + lane, pf_sdio_pio_board_pins.pins.data_gpio[lane]);
    }
}
