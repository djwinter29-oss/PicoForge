#ifndef PF_SYSTEM_H
#define PF_SYSTEM_H

#include <stdint.h>

#define PF_SYSTEM_MCU_RP2040 1u
#define PF_SYSTEM_MCU_RP2350 2u

/** Return the Pico SDK's RP2040/RP2350 identity value, or zero if unknown. */
uint8_t pf_system_mcu_id(void);

/** Return the current system clock frequency in hertz. */
uint32_t pf_system_clock_hz(void);

/** Return uptime in milliseconds, wrapping modulo 2^32. */
uint32_t pf_system_uptime_ms(void);

#endif