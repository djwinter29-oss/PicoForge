#include "system.h"
/**
 * @file system.c
 * @brief System information and runtime clock control.
 */
#include "system.h"

#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"

uint8_t pf_system_mcu_id(void)
{
#if defined(PICO_RP2350) && PICO_RP2350
    return PF_SYSTEM_MCU_RP2350;
#elif defined(PICO_RP2040) && PICO_RP2040
    return PF_SYSTEM_MCU_RP2040;
#else
    return 0u;
#endif
}

uint32_t pf_system_clock_hz(void)
{
    return clock_get_hz(clk_sys);
}

uint32_t pf_system_uptime_ms(void)
{
    return (uint32_t)to_ms_since_boot(get_absolute_time());
}

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