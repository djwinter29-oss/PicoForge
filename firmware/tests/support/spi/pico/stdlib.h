#pragma once
#include <stdint.h>
#include "hardware/gpio.h"
uint32_t time_us_32(void);
static inline uint64_t time_us_64(void) { return time_us_32(); }
static inline void busy_wait_us_32(unsigned us) { (void)us; }
static inline void sleep_ms(unsigned milliseconds) { (void)milliseconds; }
