/**
 * @file system.h
 * @brief System information, reset cause and runtime clock control.
 */
#ifndef PF_SYSTEM_H
#define PF_SYSTEM_H

#include <stdbool.h>
#include <stdint.h>

/** MCU identity values returned by pf_system_mcu_id(). */
#define PF_SYSTEM_MCU_RP2040 1u /**< RP2040 build. */
#define PF_SYSTEM_MCU_RP2350 2u /**< RP2350 build. */

/** HID reset-cause values returned by pf_system_reset_cause(). */
#define PF_SYSTEM_RESET_CAUSE_UNKNOWN 0u /**< No watchdog evidence. */
#define PF_SYSTEM_RESET_CAUSE_WATCHDOG 2u /**< Watchdog timeout or software-requested reboot. */

/** Return the Pico SDK's RP2040/RP2350 identity value, or zero if unknown. */
uint8_t pf_system_mcu_id(void);

/** Return the current system clock frequency in hertz. */
uint32_t pf_system_clock_hz(void);

/** Return uptime in milliseconds, wrapping modulo 2^32. */
uint32_t pf_system_uptime_ms(void);

/**
 * @brief Return the HID reset cause using SDK watchdog evidence.
 * @return PF_SYSTEM_RESET_CAUSE_WATCHDOG for a watchdog or software-requested reboot, otherwise
 *         PF_SYSTEM_RESET_CAUSE_UNKNOWN. The two watchdog sources are not distinguished.
 */
uint8_t pf_system_reset_cause(void);

/**
 * @brief Read and apply the saved system clock at the start of main(), before application peripherals and scheduling.
 *
 * Does nothing for 0 (keep the build clock), for a frequency the SDK cannot derive from the crystal, and when the
 * clock already runs at the requested rate. Never changes the core voltage, so clocks above the SDK default rely on
 * the voltage already configured. Keeps clk_peri on clk_sys and refreshes clk_hstx on RP2350.
 * Not hardware-qualified (record measurements under docs/tests/records and link them here); 150 MHz on RP2040 and
 * 200/250 MHz are out of specification. If 250 MHz is unstable on a board, treat that profile as unsupported there.
 * @return true if the clock was changed; false if nothing was done, without saying why.
 */
bool pf_system_clock_apply(void);

/** Reboot the MCU through the SDK watchdog. This call does not return. */
void pf_system_reset(void);

#endif
