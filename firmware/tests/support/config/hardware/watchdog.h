#ifndef HARDWARE_WATCHDOG_H
#define HARDWARE_WATCHDOG_H

#include <stdint.h>

void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms);

#endif