// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

#ifndef PF_TEST_SDIO_HARDWARE_CLOCKS_H
#define PF_TEST_SDIO_HARDWARE_CLOCKS_H

#include <stdint.h>

enum { clk_sys = 0 };
uint32_t clock_get_hz(unsigned clock);

#endif
