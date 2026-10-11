#include "system.h"

#include "hardware/clocks.h"
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