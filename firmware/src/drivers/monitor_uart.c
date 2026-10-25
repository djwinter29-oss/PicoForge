#include "monitor_uart.h"

#include <stdbool.h>
#include "hardware/uart.h"
#include "pico/stdio_uart.h"

static bool monitor_uart_initialized;

void pf_monitor_uart_init(void)
{
    if (monitor_uart_initialized) {
        return;
    }
    const pf_monitor_uart_board_t *config = &pf_monitor_uart_board;
    stdio_uart_init_full(config->uart_index == 0u ? uart0 : uart1, config->baud_rate,
                         (int)config->tx_gpio, (int)config->rx_gpio);
    /* Keep USB CDC free for the CLI. */
    stdio_filter_driver(&stdio_uart);
    monitor_uart_initialized = true;
}
