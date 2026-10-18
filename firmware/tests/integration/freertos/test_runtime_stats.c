#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/runtime")
TEST_INCLUDE_PATH("firmware/tests/support/storage")
#define __end__ test_end
#define __scratch_x_start__ test_scratch
#define __scratch_x_end__ test_scratch
#define __scratch_y_start__ test_scratch
#define __scratch_y_end__ test_scratch
#define __StackTop test_stack
#define __StackBottom test_stack
#define __StackOneTop test_stack
#define __StackOneBottom test_stack
#define RUNTIME_SOURCE "../../../src/integration/freertos/runtime_stats.c"
#include RUNTIME_SOURCE

char test_end[1], test_scratch[1], test_stack[1];
static uint64_t clock_us, idle_us;
uint64_t time_us_64(void) { return clock_us; }
uint64_t ulTaskGetIdleRunTimeCounter(void) { return idle_us; }

void setUp(void)
{
    previous_time = previous_idle = 0u;
    sampled = false;
    cpu_usage = UINT16_MAX;
    clock_us = idle_us = 0u;
}
void tearDown(void) {}

void test_first_and_short_samples_are_unavailable_then_cache_the_interval_result(void)
{
    pf_runtime_stats_t stats;
    pf_runtime_stats_read(&stats);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, stats.cpu_usage_basis_points);
    TEST_ASSERT_EQUAL_UINT32(512u, stats.ram_used_bytes);
    TEST_ASSERT_EQUAL_UINT32(1024u, stats.ram_total_bytes);
    clock_us = 249999u;
    idle_us = 200000u;
    pf_runtime_stats_read(&stats);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, stats.cpu_usage_basis_points);
    clock_us = 250000u;
    pf_runtime_stats_read(&stats);
    TEST_ASSERT_EQUAL_UINT16(2000u, stats.cpu_usage_basis_points);
    ++clock_us;
    pf_runtime_stats_read(&stats);
    TEST_ASSERT_EQUAL_UINT16(2000u, stats.cpu_usage_basis_points);
}

void test_idle_and_fully_busy_intervals_are_zero_and_one_hundred_percent(void)
{
    pf_runtime_stats_t stats;
    pf_runtime_stats_read(&stats);
    clock_us = idle_us = 250000u;
    pf_runtime_stats_read(&stats);
    TEST_ASSERT_EQUAL_UINT16(0u, stats.cpu_usage_basis_points);
    clock_us += 250000u;
    pf_runtime_stats_read(&stats);
    TEST_ASSERT_EQUAL_UINT16(10000u, stats.cpu_usage_basis_points);
}

void test_long_uptime_and_new_intervals_use_64_bit_deltas_not_boot_averages(void)
{
    pf_runtime_stats_t stats;
    clock_us = UINT64_C(1) << 40u;
    idle_us = clock_us / 2u;
    pf_runtime_stats_read(&stats);
    clock_us += 1000000u;
    idle_us += 750000u;
    pf_runtime_stats_read(&stats);
    TEST_ASSERT_EQUAL_UINT16(2500u, stats.cpu_usage_basis_points);
    clock_us += 250000u;
    pf_runtime_stats_read(&stats);
    TEST_ASSERT_EQUAL_UINT16(10000u, stats.cpu_usage_basis_points);
}
