#ifndef PF_CONFIG_SYSTEM_CLOCK_H
#define PF_CONFIG_SYSTEM_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Apply a persisted system clock during SDK runtime init (after clocks, before peripheral resets are released).
 *
 * Does nothing for 0 (keep the build clock), for a frequency the SDK cannot derive from the crystal, and when the
 * clock already runs at the requested rate. Raises the core voltage first only if needed (1.15 V up to 200 MHz,
 * 1.30 V above) and never lowers it. Keeps clk_peri on clk_sys (the SDK default for set_sys_clock_pll() is
 * 48 MHz, which would cap SPI at 24 MHz) and refreshes clk_hstx on RP2350. Not hardware-qualified; 150 MHz on
 * RP2040 and 200/250 MHz are out of specification.
 * @param system_clock_mhz Validated pf_settings_t::system_clock_mhz.
 * @return true if the clock was changed.
 */
bool pf_system_clock_apply(uint16_t system_clock_mhz);

#endif
