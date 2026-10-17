#pragma once
#include "../serial_fake.h"
#define clk_sys 0u
static inline uint32_t clock_get_hz(uint clock) { (void)clock; return 125000000u; }
