// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

#pragma once
#include "unity.h"
#define configASSERT(condition) TEST_ASSERT_TRUE(condition)
#define portMAX_DELAY 0xffff
#define pdTRUE 1
typedef int BaseType_t;
