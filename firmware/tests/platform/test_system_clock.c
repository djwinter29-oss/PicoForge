#include "unity.h"

TEST_INCLUDE_PATH("firmware/tests/support/config")

#include "platform/system.h"
#define BOOT_CONFIG_HEADER "platform/boot_config.h"
#include BOOT_CONFIG_HEADER
#include "clock_model.h"

static uint32_t sys_hz, peri_hz, hstx_hz;
static uint32_t reset_pc, reset_sp, reset_delay_ms;
static char call_log[16];
static unsigned log_len;
static bool derivable;
static bool watchdog_reset;
static pf_sysclk_profile_t saved_profile;

pf_sysclk_profile_t pf_boot_config_sysclk(void) { return saved_profile; }

bool watchdog_caused_reboot(void) { return watchdog_reset; }

uint32_t clock_get_hz(enum clock_num_rp clock) { return clock == clk_sys ? sys_hz : 0u; }
bool check_sys_clock_khz(uint32_t khz, uint *vco, uint *pd1, uint *pd2)
{
    *vco = khz * 1000u * 6u;
    *pd1 = 6u;
    *pd2 = 1u;
    return derivable;
}
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
    reset_pc = reset_sp = reset_delay_ms = 0u;
    log_len = 0u;
    derivable = true;
    watchdog_reset = false;
    saved_profile = PF_SYSCLK_DEFAULT;
    for (unsigned i = 0u; i < sizeof(call_log); ++i) {
        call_log[i] = 0;
    }
}
void tearDown(void) {}

void test_reset_cause_uses_watchdog_evidence_without_assuming_power_on(void)
{
    TEST_ASSERT_EQUAL_UINT8(PF_SYSTEM_RESET_CAUSE_UNKNOWN, pf_system_reset_cause());
    watchdog_reset = true;
    TEST_ASSERT_EQUAL_UINT8(PF_SYSTEM_RESET_CAUSE_WATCHDOG, pf_system_reset_cause());
    TEST_ASSERT_EQUAL_UINT8(2u, pf_system_reset_cause()); /* HID protocol value */
}

void test_default_zero_keeps_the_build_clock_untouched(void)
{
    TEST_ASSERT_FALSE(pf_system_clock_apply());
    TEST_ASSERT_EQUAL_UINT32(125000000u, sys_hz);
    TEST_ASSERT_EQUAL_UINT(0u, log_len);
}

void test_underivable_clock_and_same_clock_change_nothing(void)
{
    derivable = false;
    saved_profile = PF_SYSCLK_200MHZ;
    TEST_ASSERT_FALSE(pf_system_clock_apply());
    derivable = true;
    sys_hz = 200000000u;
    TEST_ASSERT_FALSE(pf_system_clock_apply());
    TEST_ASSERT_EQUAL_UINT(0u, log_len);
}

void test_clock_switch_never_touches_voltage_and_keeps_peri_on_sys(void)
{
    saved_profile = PF_SYSCLK_150MHZ;
    TEST_ASSERT_TRUE(pf_system_clock_apply());
    TEST_ASSERT_EQUAL_STRING("pPH", call_log); /* no voltage call: no vreg stub exists */
    TEST_ASSERT_EQUAL_UINT32(150000000u, sys_hz);
    TEST_ASSERT_EQUAL_UINT32(150000000u, peri_hz);
    TEST_ASSERT_EQUAL_UINT32(150000000u, hstx_hz);
}

void test_250mhz_applies_like_any_other_profile(void)
{
    saved_profile = PF_SYSCLK_250MHZ;
    TEST_ASSERT_TRUE(pf_system_clock_apply());
    TEST_ASSERT_EQUAL_UINT32(250000000u, sys_hz);
    TEST_ASSERT_EQUAL_UINT32(250000000u, peri_hz);
}

void test_system_reset_reboots_without_delay_or_boot_address_override(void)
{
    pf_system_reset();
    TEST_ASSERT_EQUAL_STRING("r", call_log);
    TEST_ASSERT_EQUAL_UINT32(0u, reset_pc);
    TEST_ASSERT_EQUAL_UINT32(0u, reset_sp);
    TEST_ASSERT_EQUAL_UINT32(0u, reset_delay_ms);
}
