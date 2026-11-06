// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

#ifndef PF_TEST_DCD_PLATFORM_H
#define PF_TEST_DCD_PLATFORM_H

#include <stddef.h>
#include <assert.h>
#include "hardware/irq.h"

#define NUM_CORES 2u
#define hard_assert assert
uint get_core_num(void);

#endif
