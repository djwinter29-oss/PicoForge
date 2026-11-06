// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

#ifndef STORAGE_TEST_STDLIB_H
#define STORAGE_TEST_STDLIB_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
void sleep_ms(uint32_t ms);
void busy_wait_ms(uint32_t ms);
void panic(const char *format, ...);
#endif
