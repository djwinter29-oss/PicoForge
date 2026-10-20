#ifndef PICO_FLASH_H
#define PICO_FLASH_H

#include <stdint.h>

#include "hardware/flash.h"

int flash_safe_execute(void (*func)(void *), void *param, uint32_t enter_exit_timeout_ms);

#endif
