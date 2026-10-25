#include "unity.h"

TEST_INCLUDE_PATH("firmware/tests/support/config")

#include "drivers/system.h"
#include "clock_model.h"

static uint32_t sys_hz, peri_hz, hstx_hz, wait_cycles;
static uint32_t reset_pc, reset_sp, reset_delay_ms;
static enum vreg_voltage voltage;
static char call_log[16];
static unsigned log_len;
static bool derivable;
static bool watchdog_reset;

bool watchdog_caused_reboot(void) { return watchdog_reset; }

uint32_t clock_get_hz(enum clock_num_rp clock) { return clock == clk_sys ? sys_hz : 0u; }
bool check_sys_clock_khz(uint32_t khz, uint *vco, uint *pd1, uint *pd2)
{
    *vco = khz * 1000u * 6u;
    *pd1 = 6u;
    *pd2 = 1u;
    return derivable;
}
void vreg_set_voltage(enum vreg_voltage v) { voltage = v; call_log[log_len++] = 'v'; }
enum vreg_voltage vreg_get_voltage(void) { return voltage; }
void busy_wait_at_least_cycles(uint32_t cycles) { wait_cycles = cycles; call_log[log_len++] = 'w'; }
void set_sys_clock_pll(uint32_t vco, uint pd1, uint pd2)
{
    sys_hz = vco / (pd1 * pd2);
    peri_hz = 48000000u; /* SDK default */
    call_log[log_len++] = 'p';
}
bool clock_configure_undivided(enum clock_num_rp clock, uint32_t src, uint32_t aux, uint32_t hz)
{
    (void)src;
    (void)aux;
    if (clock == clk_peri) {
        peri_hz = hz;
        call_log[log_len++] = 'P';
    }
    if (clock == clk_hstx) {
        hstx_hz = hz;
        call_log[log_len++] = 'H';
    }
    return true;
}
void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms)
{
    reset_pc = pc;
    reset_sp = sp;
    reset_delay_ms = delay_ms;
    call_log[log_len++] = 'r';
}

void setUp(void)
{
    sys_hz = peri_hz = hstx_hz = 125000000u;
    wait_cycles = 0u;
    reset_pc = reset_sp = reset_delay_ms = 0u;
    voltage = VREG_VOLTAGE_1_10;
    log_len = 0u;
    derivable = true;
    watchdog_reset = false;
    for (unsigned i = 0u; i < sizeof(call_log); ++i) {
        call_log[i] = 0;
    }
}
void tearDown(void) {}

void test_reset_cause_uses_watchdog_evidence_without_assuming_power_on(void)
{
    TEST_ASSERT_EQUAL_UINT8(0u, pf_system_reset_cause());
    watchdog_reset = true;
    TEST_ASSERT_EQUAL_UINT8(2u, pf_system_reset_cause());
}

void test_default_zero_keeps_the_build_clock_untouched(void)
{
    TEST_ASSERT_FALSE(pf_system_clock_apply(0u));
    TEST_ASSERT_EQUAL_UINT32(125000000u, sys_hz);
    TEST_ASSERT_EQUAL_UINT(0u, log_len);
}

void test_underivable_clock_and_same_clock_change_nothing(void)
{
    derivable = false;
    TEST_ASSERT_FALSE(pf_system_clock_apply(200u));
    derivable = true;
    sys_hz = 200000000u;
    TEST_ASSERT_FALSE(pf_system_clock_apply(200u));
    TEST_ASSERT_EQUAL_UINT(0u, log_len);
}

void test_up_to_200mhz_raises_to_1v15_settles_then_switches_and_keeps_peri_on_sys(void)
{
    TEST_ASSERT_TRUE(pf_system_clock_apply(150u));
    TEST_ASSERT_EQUAL_STRING("vwpPH", call_log);
    TEST_ASSERT_EQUAL_INT(VREG_VOLTAGE_1_15, voltage);
    TEST_ASSERT_EQUAL_UINT32(125000u, wait_cycles); /* 1000 us at 125 MHz */
    TEST_ASSERT_EQUAL_UINT32(150000000u, sys_hz);
    TEST_ASSERT_EQUAL_UINT32(150000000u, peri_hz);
    TEST_ASSERT_EQUAL_UINT32(150000000u, hstx_hz);
}

void test_above_200mhz_needs_1v30(void)
{
    TEST_ASSERT_TRUE(pf_system_clock_apply(250u));
    TEST_ASSERT_EQUAL_INT(VREG_VOLTAGE_1_30, voltage);
    TEST_ASSERT_EQUAL_UINT32(250000000u, peri_hz);
}

void test_voltage_is_raised_only_when_needed_and_never_lowered(void)
{
    voltage = VREG_VOLTAGE_1_30; /* active compile-time overclock */
    sys_hz = 250000000u;
    TEST_ASSERT_TRUE(pf_system_clock_apply(150u));
    TEST_ASSERT_EQUAL_INT(VREG_VOLTAGE_1_30, voltage);
    TEST_ASSERT_EQUAL_STRING("pPH", call_log);
}

void test_system_reset_reboots_without_delay_or_boot_address_override(void)
{
    pf_system_reset();
    TEST_ASSERT_EQUAL_STRING("r", call_log);
    TEST_ASSERT_EQUAL_UINT32(0u, reset_pc);
    TEST_ASSERT_EQUAL_UINT32(0u, reset_sp);
    TEST_ASSERT_EQUAL_UINT32(0u, reset_delay_ms);
}
