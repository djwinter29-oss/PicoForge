#pragma once
#include "serial_fake.h"
#define clk_sys 0u
static inline uint32_t clock_get_hz(uint clock)
{ (void)clock; return serial_fake.system_hz != 0u ? serial_fake.system_hz : 125000000u; }
