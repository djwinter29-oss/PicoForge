#pragma once
#include "../hardware/gpio.h"
static inline uint64_t time_us_64(void) { serial_fake.now += 50000u; return serial_fake.now; }
static inline void busy_wait_us_32(uint us) { (void)us; }
