// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

#ifndef HARDWARE_WATCHDOG_H
#define HARDWARE_WATCHDOG_H

#include <stdint.h>
#include <stdbool.h>

void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms);
bool watchdog_caused_reboot(void);

#endif