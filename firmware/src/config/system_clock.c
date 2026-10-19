/**
 * @file system_clock.c
 * @brief Runtime system-clock change using public SDK clock and regulator APIs (no vendored SDK code).
 *
 * Runs from a runtime-init HW hook ordered after the SDK's clock init, so peripherals are still in reset, the ticks
 * (clk_ref, unchanged) are running and FreeRTOS later reads the new clk_sys for its SysTick.
 */
#include "config/system_clock.h"

#include "hardware/clocks.h"
#include "hardware/vreg.h"

bool pf_system_clock_apply(uint16_t system_clock_mhz)
{
    uint vco_hz, post_div1, post_div2;
    const uint32_t target_hz = (uint32_t)system_clock_mhz * 1000000u;
    const uint32_t current_hz = clock_get_hz(clk_sys);

    if (system_clock_mhz == 0u || target_hz == current_hz ||
        !check_sys_clock_khz((uint32_t)system_clock_mhz * 1000u, &vco_hz, &post_div1, &post_div2)) {
        return false;
    }
    if (target_hz > current_hz) {
        const enum vreg_voltage needed = system_clock_mhz > 200u ? VREG_VOLTAGE_1_30 : VREG_VOLTAGE_1_15;
        if (vreg_get_voltage() < needed) {
            vreg_set_voltage(needed);
            /* TIMER is still in reset here, so settle by CPU cycles at the current (known) clk_sys. */
            busy_wait_at_least_cycles(
                (uint32_t)((SYS_CLK_VREG_VOLTAGE_AUTO_ADJUST_DELAY_US * (uint64_t)current_hz) / 1000000u));
        }
    }
    set_sys_clock_pll(vco_hz, post_div1, post_div2);
    /* set_sys_clock_pll() moves clk_peri to 48 MHz; restore the SDK-boot behavior (clk_peri = clk_sys). */
    clock_configure_undivided(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, target_hz);
#if HAS_HSTX
    /* clk_hstx follows clk_sys; re-run its configuration so its recorded frequency is correct. */
    clock_configure_undivided(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, target_hz);
#endif
    return true;
}
