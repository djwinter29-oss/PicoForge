// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

#ifndef PF_TEST_RUNTIME_ADDRESSMAP_H
#define PF_TEST_RUNTIME_ADDRESSMAP_H
#include <stdint.h>
extern char test_end[];
#define SRAM_BASE ((uintptr_t)test_end - 512u)
#define SRAM_END (SRAM_BASE + 1024u)
#endif
