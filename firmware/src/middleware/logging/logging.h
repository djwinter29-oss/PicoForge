#ifndef PF_LOGGING_H
#define PF_LOGGING_H

#include <stdbool.h>

typedef enum {
    PF_LOG_LEVEL_DEBUG = 0,
    PF_LOG_LEVEL_INFO,
    PF_LOG_LEVEL_WARNING,
    PF_LOG_LEVEL_ERROR,
} log_level_t;

/** Initialize SDK stdio on UART0. Call once before starting application tasks. */
bool pf_log_init(void);

/** Set the minimum emitted severity; this may be changed while tasks are running. */
bool pf_log_set_minimum_level(log_level_t level);

/** Emit one serialized log line. This blocking API is not safe to call from an ISR. */
void pf_log_write(log_level_t level, const char *tag, const char *format, ...);

#define PF_LOG_DEBUG(tag, ...) \
    pf_log_write(PF_LOG_LEVEL_DEBUG, tag, __VA_ARGS__)
#define PF_LOG_INFO(tag, ...) \
    pf_log_write(PF_LOG_LEVEL_INFO, tag, __VA_ARGS__)
#define PF_LOG_WARNING(tag, ...) \
    pf_log_write(PF_LOG_LEVEL_WARNING, tag, __VA_ARGS__)
#define PF_LOG_ERROR(tag, ...) \
    pf_log_write(PF_LOG_LEVEL_ERROR, tag, __VA_ARGS__)

#endif