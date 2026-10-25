/** @file
 * @brief Build-selected board assignments; each object is declared extern in its driver header.
 */
#include "drivers/led.h"
#include "drivers/monitor_uart.h"
#include "config/config.h"

const pf_led_board_t pf_led_board = {
    .gpio = PF_BOARD_LED_GPIO,
    .active_high = !PICO_DEFAULT_LED_PIN_INVERTED,
};

const pf_monitor_uart_board_t pf_monitor_uart_board = {
    .uart_index = PICO_DEFAULT_UART,
    .tx_gpio = PICO_DEFAULT_UART_TX_PIN,
    .rx_gpio = PICO_DEFAULT_UART_RX_PIN,
    .baud_rate = 115200u,
};
