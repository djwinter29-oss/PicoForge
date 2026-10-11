#include "logging.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>

#include "pico/stdio.h"
#include "pico/stdio_uart.h"

enum { PF_LOG_LINE_SIZE = 256 };

static atomic_int minimum_level = ATOMIC_VAR_INIT(PF_LOG_LEVEL_INFO);
static bool initialized;

void pf_log_init(void)
{
    if (initialized) {
        return;
    }

    stdio_filter_driver(&stdio_uart);
    initialized = true;
}

bool pf_log_set_minimum_level(log_level_t level)
{
    if ((level < PF_LOG_LEVEL_DEBUG) || (level > PF_LOG_LEVEL_ERROR)) {
        return false;
    }

    atomic_store(&minimum_level, (int)level);
    return true;
}

void pf_log_write(log_level_t level, const char *tag, const char *format, ...)
{
    static const char *const level_names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    char line[PF_LOG_LINE_SIZE];
    int prefix_length;
    int message_length;
    size_t line_length;
    va_list arguments;

    if (!initialized || (format == NULL) ||
        (level < PF_LOG_LEVEL_DEBUG) || (level > PF_LOG_LEVEL_ERROR) ||
        ((int)level < atomic_load(&minimum_level))) {
        return;
    }

    prefix_length = snprintf(line, sizeof(line), "[%s] %s: ", level_names[level],
                             (tag != NULL) ? tag : "picoforge");
    if ((prefix_length < 0) || ((size_t)prefix_length >= sizeof(line))) {
        return;
    }

    va_start(arguments, format);
    message_length = vsnprintf(line + prefix_length, sizeof(line) - (size_t)prefix_length,
                               format, arguments);
    va_end(arguments);
    if (message_length < 0) {
        return;
    }

    line_length = (size_t)prefix_length + (size_t)message_length;
    if (line_length >= sizeof(line)) {
        line_length = sizeof(line) - 1u;
    }
    (void)stdio_put_string(line, (int)line_length, true, true);
}