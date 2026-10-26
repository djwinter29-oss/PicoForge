#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/config.h"
#include "platform/logging.h"
#include "pico/stdio.h"
#include "pico/stdio_uart.h"

#ifndef PF_TEST_LOGGING_DEFAULT_LEVEL
#define PF_TEST_LOGGING_DEFAULT_LEVEL PF_LOGGING_LEVEL_WARNING
#endif
#ifndef PF_TEST_LOGGING_UART_INDEX
#define PF_TEST_LOGGING_UART_INDEX 1u
#endif
#ifndef PF_TEST_LOGGING_TX_GPIO
#define PF_TEST_LOGGING_TX_GPIO 4u
#endif
#ifndef PF_TEST_LOGGING_BAUD_RATE
#define PF_TEST_LOGGING_BAUD_RATE 57600u
#endif

struct uart_inst {
    unsigned index;
};

static struct uart_inst uart0_instance = {.index = 0u};
static struct uart_inst uart1_instance = {.index = 1u};
uart_inst_t *uart0 = &uart0_instance;
uart_inst_t *uart1 = &uart1_instance;
stdio_driver_t stdio_uart;
const pf_logging_board_t pf_logging_board = {
    .uart_index = PF_TEST_LOGGING_UART_INDEX,
    .tx_gpio = PF_TEST_LOGGING_TX_GPIO,
    .rx_gpio = 5u,
    .default_level = PF_TEST_LOGGING_DEFAULT_LEVEL,
    .baud_rate = PF_TEST_LOGGING_BAUD_RATE,
};

static unsigned init_calls;
static unsigned filter_calls;
static uart_inst_t *initialized_uart;
static uint32_t initialized_baud;
static int initialized_tx;
static int initialized_rx;
static stdio_driver_t *filtered_driver;
static unsigned output_calls;
static int output_result = -1;
static bool output_newline;
static bool output_cr_translation;
static char output[PF_LOG_LINE_BUFFER_SIZE + 1u];
static size_t output_length;

void stdio_uart_init_full(uart_inst_t *uart, uint32_t baud_rate, int tx_gpio, int rx_gpio)
{
    ++init_calls;
    initialized_uart = uart;
    initialized_baud = baud_rate;
    initialized_tx = tx_gpio;
    initialized_rx = rx_gpio;
}

void stdio_filter_driver(stdio_driver_t *driver)
{
    ++filter_calls;
    filtered_driver = driver;
}

int stdio_put_string(const char *text, int length, bool newline, bool cr_translation)
{
    ++output_calls;
    output_newline = newline;
    output_cr_translation = cr_translation;
    if (output_result == 0) {
        return 0;
    }

    output_length = (size_t)length;
    assert(output_length + (newline ? 1u : 0u) < sizeof(output));
    memcpy(output, text, output_length);
    if (newline) {
        output[output_length++] = '\n';
    }
    output[output_length] = '\0';
    return output_result >= 0 ? output_result : length;
}

void panic(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    abort();
}

int main(void)
{
    assert(!PF_LOGGING_WARNING("test", "before init"));
    assert(output_calls == 0u);

    pf_logging_init();
    pf_logging_init();
    assert(init_calls == 1u);
    assert(initialized_uart == uart1);
    assert(initialized_baud == 57600u);
    assert(initialized_tx == 4 && initialized_rx == 5);
    assert(filter_calls == 1u && filtered_driver == &stdio_uart);

    assert(!PF_LOGGING_INFO("test", "filtered"));
    assert(output_calls == 0u);
    assert(PF_LOGGING_WARNING("test", "value=%u", 7u));
    assert(output_calls == 1u);
    assert(output_length == strlen("[WARN] test: value=7\n"));
    assert(strcmp(output, "[WARN] test: value=7\n") == 0);
    assert(output_newline && output_cr_translation);

    assert(PF_LOGGING_ERROR(NULL, "fallback"));
    assert(strcmp(output, "[ERROR] picoforge: fallback\n") == 0);
    assert(!pf_logging_write((pf_logging_level_t)99, "test", "invalid"));
    assert(!pf_logging_write(PF_LOGGING_LEVEL_ERROR, "test", NULL));

    assert(PF_LOGGING_WARNING("0123456789abcdefTRUNCATED", "m"));
    assert(strcmp(output, "[WARN] 0123456789abcdef: m\n") == 0);

    char long_message[PF_LOG_LINE_BUFFER_SIZE + 32u];
    memset(long_message, 'x', sizeof(long_message) - 1u);
    long_message[sizeof(long_message) - 1u] = '\0';
    assert(PF_LOGGING_ERROR("long", "%s", long_message));
    assert(output_length == PF_LOG_LINE_BUFFER_SIZE);
    assert(output[PF_LOG_LINE_BUFFER_SIZE - 4u] == '.');
    assert(output[PF_LOG_LINE_BUFFER_SIZE - 3u] == '.');
    assert(output[PF_LOG_LINE_BUFFER_SIZE - 2u] == '.');
    assert(output[PF_LOG_LINE_BUFFER_SIZE - 1u] == '\n');

    output_result = 0;
    size_t previous_output_length = output_length;
    assert(!PF_LOGGING_ERROR("dropped", "not accepted"));
    assert(output_length == previous_output_length);
    return 0;
}
