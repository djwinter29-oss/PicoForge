#ifndef CONFIG_TEST_PICO_STDLIB_H
#define CONFIG_TEST_PICO_STDLIB_H

#include <stdint.h>

typedef struct {
    uint64_t us_since_boot;
} absolute_time_t;

static inline absolute_time_t get_absolute_time(void)
{
    return (absolute_time_t){0u};
}

static inline uint32_t to_ms_since_boot(absolute_time_t time)
{
    return (uint32_t)(time.us_since_boot / 1000u);
}

#endif
