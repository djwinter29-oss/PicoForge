#pragma once
#include "../hardware/gpio.h"
static inline uint64_t time_us_64(void) { serial_fake.now += 50000u; return serial_fake.now; }
static inline void busy_wait_us_32(uint us) { (void)us; }
static inline void sleep_ms(uint32_t milliseconds) { serial_fake.now += (uint64_t)milliseconds * 1000u; }
