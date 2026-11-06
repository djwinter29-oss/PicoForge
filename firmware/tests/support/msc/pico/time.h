// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

#ifndef PF_MSC_TEST_PICO_TIME_H
#define PF_MSC_TEST_PICO_TIME_H
#include <stdint.h>
extern uint64_t test_time_us;
static inline uint64_t time_us_64(void) { return test_time_us; }
#endif
