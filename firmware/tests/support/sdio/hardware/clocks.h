#ifndef PF_TEST_SDIO_HARDWARE_CLOCKS_H
#define PF_TEST_SDIO_HARDWARE_CLOCKS_H

#include <stdint.h>

enum { clk_sys = 0 };
uint32_t clock_get_hz(unsigned clock);

#endif
