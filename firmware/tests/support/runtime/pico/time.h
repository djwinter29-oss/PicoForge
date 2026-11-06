// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

#ifndef PF_TEST_RUNTIME_TIME_H
#define PF_TEST_RUNTIME_TIME_H
#include <stdint.h>
typedef uint64_t absolute_time_t;
uint64_t time_us_64(void);
absolute_time_t get_absolute_time(void);
uint64_t to_ms_since_boot(absolute_time_t value);
#endif
