#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/runtime")
TEST_INCLUDE_PATH("firmware/tests/support/storage")
#include "FreeRTOS.h"
#include "task.h"
#define __end__ test_end
#define __scratch_x_start__ test_scratch
#define __scratch_x_end__ test_scratch
#define __scratch_y_start__ test_scratch
#define __scratch_y_end__ test_scratch
#define __StackTop test_stack
#define __StackBottom test_stack
#define __StackOneTop test_stack
#define __StackOneBottom test_stack
#define configNUMBER_OF_CORES 2
TaskHandle_t xTaskGetIdleTaskHandleForCore(BaseType_t core);
uint64_t ulTaskGetRunTimeCounter(TaskHandle_t task);
#define RUNTIME_SOURCE "app/runtime_stats.c"
#include RUNTIME_SOURCE

char test_end[1], test_scratch[1], test_stack[1];
static uint64_t clock_us, idle_us[PF_RUNTIME_CORE_COUNT];
uint64_t time_us_64(void) { return clock_us; }
TaskHandle_t xTaskGetIdleTaskHandleForCore(BaseType_t core)
{
    return (TaskHandle_t)(uintptr_t)((unsigned)core + 1u);
}
uint64_t ulTaskGetRunTimeCounter(TaskHandle_t task)
{
    return idle_us[(uintptr_t)task - 1u];
}

void setUp(void)
{
    previous_time = 0u;
    previous_idle[0] = previous_idle[1] = 0u;
    sampled = false;
    cpu_usage[0] = cpu_usage[1] = UINT16_MAX;
    clock_us = idle_us[0] = idle_us[1] = 0u;
}
void tearDown(void) {}

void test_first_and_short_samples_are_unavailable_then_cache_the_interval_result(void)
{
    pf_runtime_stats_t stats;
    pf_runtime_stats_read(&stats);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, stats.cpu_usage_basis_points[0]);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, stats.cpu_usage_basis_points[1]);
    TEST_ASSERT_EQUAL_UINT32(512u, stats.ram_used_bytes);
    TEST_ASSERT_EQUAL_UINT32(1024u, stats.ram_total_bytes);
    clock_us = 249999u;
    idle_us[0] = 200000u;
    idle_us[1] = 225000u;
    pf_runtime_stats_read(&stats);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, stats.cpu_usage_basis_points[0]);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, stats.cpu_usage_basis_points[1]);
    clock_us = 250000u;
    pf_runtime_stats_read(&stats);
    TEST_ASSERT_EQUAL_UINT16(2000u, stats.cpu_usage_basis_points[0]);
    TEST_ASSERT_EQUAL_UINT16(1000u, stats.cpu_usage_basis_points[1]);
    ++clock_us;
    pf_runtime_stats_read(&stats);
    TEST_ASSERT_EQUAL_UINT16(2000u, stats.cpu_usage_basis_points[0]);
    TEST_ASSERT_EQUAL_UINT16(1000u, stats.cpu_usage_basis_points[1]);
}

void test_idle_and_fully_busy_intervals_are_zero_and_one_hundred_percent(void)
{
    pf_runtime_stats_t stats;
    pf_runtime_stats_read(&stats);
    clock_us = 250000u;
    idle_us[0] = 250000u;
    idle_us[1] = 0u;
    pf_runtime_stats_read(&stats);
    TEST_ASSERT_EQUAL_UINT16(0u, stats.cpu_usage_basis_points[0]);
    TEST_ASSERT_EQUAL_UINT16(10000u, stats.cpu_usage_basis_points[1]);
    clock_us += 250000u;
    idle_us[0] = 250000u;
    idle_us[1] = 250000u;
    pf_runtime_stats_read(&stats);
    TEST_ASSERT_EQUAL_UINT16(10000u, stats.cpu_usage_basis_points[0]);
    TEST_ASSERT_EQUAL_UINT16(0u, stats.cpu_usage_basis_points[1]);
}

void test_long_uptime_and_new_intervals_use_64_bit_deltas_not_boot_averages(void)
{
    pf_runtime_stats_t stats;
    clock_us = UINT64_C(1) << 40u;
    idle_us[0] = clock_us / 2u;
    idle_us[1] = clock_us / 2u;
    pf_runtime_stats_read(&stats);
    clock_us += 1000000u;
    idle_us[0] += 900000u;
    idle_us[1] += 500000u;
    pf_runtime_stats_read(&stats);
    TEST_ASSERT_EQUAL_UINT16(1000u, stats.cpu_usage_basis_points[0]);
    TEST_ASSERT_EQUAL_UINT16(5000u, stats.cpu_usage_basis_points[1]);
    clock_us += 250000u;
    idle_us[0] += 0u;
    idle_us[1] += 0u;
    pf_runtime_stats_read(&stats);
    TEST_ASSERT_EQUAL_UINT16(10000u, stats.cpu_usage_basis_points[0]);
    TEST_ASSERT_EQUAL_UINT16(10000u, stats.cpu_usage_basis_points[1]);
}

void test_one_busy_core_and_one_idle_core_reports_fifty_percent(void)
{
    pf_runtime_stats_t stats;
    pf_runtime_stats_read(&stats);
    clock_us = 250000u;
    idle_us[0] = 125000u;
    idle_us[1] = 250000u;
    pf_runtime_stats_read(&stats);
    TEST_ASSERT_EQUAL_UINT16(5000u, stats.cpu_usage_basis_points[0]);
    TEST_ASSERT_EQUAL_UINT16(0u, stats.cpu_usage_basis_points[1]);
}

void test_adjacent_clock_sampling_skew_saturates_at_zero_usage(void)
{
    pf_runtime_stats_t stats;
    pf_runtime_stats_read(&stats);
    clock_us = 250000u;
    idle_us[0] = 250002u;
    idle_us[1] = 250002u;
    pf_runtime_stats_read(&stats);
    TEST_ASSERT_EQUAL_UINT16(0u, stats.cpu_usage_basis_points[0]);
    TEST_ASSERT_EQUAL_UINT16(0u, stats.cpu_usage_basis_points[1]);
}
