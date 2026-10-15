#pragma once
#include <stdint.h>
uint32_t time_us_32(void);
static inline void sleep_ms(unsigned ms) { (void)ms; }
