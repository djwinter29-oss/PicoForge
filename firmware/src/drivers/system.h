#ifndef PF_SYSTEM_H
#define PF_SYSTEM_H

#include <stdbool.h>
#include <stdint.h>

#define PF_SYSTEM_MCU_RP2040 1u
#define PF_SYSTEM_MCU_RP2350 2u

/** Return the Pico SDK's RP2040/RP2350 identity value, or zero if unknown. */
uint8_t pf_system_mcu_id(void);

/** Return the current system clock frequency in hertz. */
uint32_t pf_system_clock_hz(void);

/** Return uptime in milliseconds, wrapping modulo 2^32. */
uint32_t pf_system_uptime_ms(void);

/**
 * @brief Return the HID reset cause using SDK watchdog evidence.
 * @return 2 for watchdog reset (including software-requested reboot), otherwise 0 (unknown).
 */
uint8_t pf_system_reset_cause(void);

/**
 * @brief Read and apply the saved system clock at the start of main(), before application peripherals and scheduling.
 *
 * Does nothing for 0 (keep the build clock), for a frequency the SDK cannot derive from the crystal, and when the
 * clock already runs at the requested rate. Raises the core voltage first only if needed (1.15 V up to 200 MHz,
 * 1.30 V above) and never lowers it. Keeps clk_peri on clk_sys and refreshes clk_hstx on RP2350. Not
 * hardware-qualified; 150 MHz on RP2040 and 200/250 MHz are out of specification.
 * @return true if the clock was changed.
 */
bool pf_system_clock_apply(void);

/** Reboot the MCU through the SDK watchdog. This call does not return. */
void pf_system_reset(void);

#endif