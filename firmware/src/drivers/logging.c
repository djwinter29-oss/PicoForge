#include "logging.h"

#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include "config/config.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico/stdio.h"
#include "pico/stdio_uart.h"
#include "pico/stdlib.h"

#if !PICO_STDOUT_MUTEX
#error "The logging driver requires Pico SDK stdout serialization"
#endif

_Static_assert(PF_LOG_LINE_BUFFER_SIZE >= sizeof("[ERROR] picoforge: ..."),
               "Log line buffer must fit the default error prefix and ellipsis");
_Static_assert(PF_LOG_LINE_BUFFER_SIZE <= INT_MAX, "Log line buffer must fit stdio_put_string length");

static bool logging_initialized;

/*
 * ponytail: logging_initialized is a plain flag, so init must run once from startup before other cores or tasks log.
 * Upgrade path: guard with an SDK spin lock if init ever needs to be callable concurrently.
 */
void pf_logging_init(void)
{
    if (logging_initialized) {
        return;
    }
    const pf_logging_board_t *config = &pf_logging_board;
    if (config->uart_index > 1u) {
        panic("Invalid logging UART index: %u", (unsigned)config->uart_index);
    }
    if (config->baud_rate == 0u) {
        panic("Invalid logging UART baud rate: 0");
    }
    if (((config->tx_gpio != UINT8_MAX) && (config->tx_gpio >= NUM_BANK0_GPIOS)) ||
        ((config->rx_gpio != UINT8_MAX) && (config->rx_gpio >= NUM_BANK0_GPIOS))) {
        panic("Invalid logging UART GPIO: tx %u, rx %u", (unsigned)config->tx_gpio, (unsigned)config->rx_gpio);
    }
    if (config->default_level > (uint8_t)PF_LOGGING_LEVEL_ERROR) {
        panic("Invalid logging default severity: %u", (unsigned)config->default_level);
    }
    stdio_uart_init_full(config->uart_index == 0u ? uart0 : uart1, config->baud_rate,
                         config->tx_gpio == UINT8_MAX ? -1 : config->tx_gpio,
                         config->rx_gpio == UINT8_MAX ? -1 : config->rx_gpio);
    /* Keep USB CDC free for the CLI. */
    stdio_filter_driver(&stdio_uart);
    logging_initialized = true;
}

/*
 * ponytail: output is synchronous and the bool result does not say why a line was not written. Ceiling: a
 * latency-sensitive task would stall for up to ~11 ms per line. Upgrade path: queue lines to a low-priority logging
 * task and return a reason enum, once a caller needs either.
 */
bool pf_logging_write(pf_logging_level_t level, const char *tag, const char *format, ...)
{
    static const char *const level_names[] = {"INFO", "WARN", "ERROR"};
    char line[PF_LOG_LINE_BUFFER_SIZE];
    int prefix_length;
    int message_length;
    size_t line_length;
    va_list arguments;

    if (!logging_initialized || (format == NULL) ||
        (level < PF_LOGGING_LEVEL_INFO) || (level > PF_LOGGING_LEVEL_ERROR)) {
        return false;
    }
    if ((uint8_t)level < pf_logging_board.default_level) {
        return false;
    }

    prefix_length = snprintf(line, sizeof(line), "[%s] %s: ", level_names[level],
                             (tag != NULL) ? tag : "picoforge");
    if ((prefix_length < 0) || ((size_t)prefix_length >= sizeof(line) - 3u)) {
        return false;
    }

    va_start(arguments, format);
    message_length = vsnprintf(line + prefix_length, sizeof(line) - (size_t)prefix_length,
                               format, arguments);
    va_end(arguments);
    if (message_length < 0) {
        return false;
    }

    line_length = (size_t)prefix_length + (size_t)message_length;
    if (line_length >= sizeof(line)) {
        line_length = sizeof(line) - 1u;
        line[line_length - 3u] = '.';
        line[line_length - 2u] = '.';
        line[line_length - 1u] = '.';
    }
    return stdio_put_string(line, (int)line_length, true, true) == (int)line_length;
}
