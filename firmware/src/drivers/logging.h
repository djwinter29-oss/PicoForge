#ifndef PF_LOGGING_H
#define PF_LOGGING_H

#include <stdbool.h>
#include <stdint.h>

/** Log severity levels, ordered from least to most severe. */
typedef enum {
    PF_LOGGING_LEVEL_INFO = 0, /**< Normal operational events. */
    PF_LOGGING_LEVEL_WARNING, /**< Recoverable problems. */
    PF_LOGGING_LEVEL_ERROR, /**< Failed operations. */
} pf_logging_level_t;

/** Board-supplied logging output and severity configuration. */
typedef struct {
    uint8_t uart_index; /**< Monitor hardware UART index: 0 or 1. */
    uint8_t tx_gpio; /**< Monitor UART TX GPIO number, or UINT8_MAX for none. */
    uint8_t rx_gpio; /**< Monitor UART RX GPIO number, or UINT8_MAX for none. */
    uint8_t default_level; /**< Lowest severity emitted by default. */
    uint32_t baud_rate; /**< Monitor UART line rate in baud. */
} pf_logging_board_t;

/** Build-selected assignment, defined in config/board.c. */
extern const pf_logging_board_t pf_logging_board;

/**
 * @brief Initialize the board-configured logging output; later calls are no-ops.
 * @note Logging is ignored until initialization completes. Call once from startup before other cores or tasks log;
 *       initialization is not multicore-safe.
 */
void pf_logging_init(void);

/**
 * @brief Emit one serialized log line to the initialized monitor UART.
 * @param level Severity to emit.
 * @param tag Short subsystem name; NULL uses "picoforge".
 * @param format printf-style message format.
 * @return true if the line was accepted by stdio; false if filtered, invalid, uninitialized, or dropped.
 * @note This blocking API is not safe to call from an ISR. A line takes about 87 us per character
 *       at 115200 baud (about 11 ms for a maximum-length line); avoid frequent writes in latency-sensitive tasks.
 * @note Output is capped at PF_LOG_LINE_BUFFER_SIZE - 1 characters; longer messages end with an ellipsis.
 * @note The driver requires PICO_STDOUT_MUTEX so the Pico SDK serializes this call with concurrent stdio writes.
 */
#if defined(__GNUC__) || defined(__clang__)
#define PF_LOGGING_PRINTF_FORMAT(format_index, first_argument) \
    __attribute__((format(printf, format_index, first_argument)))
#else
#define PF_LOGGING_PRINTF_FORMAT(format_index, first_argument)
#endif
bool pf_logging_write(pf_logging_level_t level, const char *tag, const char *format, ...)
    PF_LOGGING_PRINTF_FORMAT(3, 4);
#undef PF_LOGGING_PRINTF_FORMAT

/** @brief Emit a normal operational log line. */
#define PF_LOGGING_INFO(tag, ...) \
    pf_logging_write(PF_LOGGING_LEVEL_INFO, tag, __VA_ARGS__)
/** @brief Emit a recoverable-problem log line. */
#define PF_LOGGING_WARNING(tag, ...) \
    pf_logging_write(PF_LOGGING_LEVEL_WARNING, tag, __VA_ARGS__)
/** @brief Emit a failed-operation log line. */
#define PF_LOGGING_ERROR(tag, ...) \
    pf_logging_write(PF_LOGGING_LEVEL_ERROR, tag, __VA_ARGS__)

#endif
