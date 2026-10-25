#include "unity.h"

#include <string.h>

TEST_INCLUDE_PATH("firmware/tests/support/config")

#include "FreeRTOS.h"
#include "task.h"
#include "platform/boot_config_store.h"

uint8_t pf_test_flash[PICO_FLASH_SIZE_BYTES];
unsigned pf_test_erase_calls;
int pf_test_safe_execute_result;
static unsigned program_calls;
static unsigned safe_calls;
static bool skip_program;
static BaseType_t scheduler_state;
static UBaseType_t task_count;
static UBaseType_t task_count_after_lockout;
static unsigned reclaim_polls;
static void (*during_flash_op)(void);
int pf_test_in_isr;

BaseType_t xPortIsInsideInterrupt(void) { return pf_test_in_isr; }
BaseType_t xTaskGetSchedulerState(void) { return scheduler_state; }
UBaseType_t uxTaskGetNumberOfTasks(void) { return task_count; }

/* The idle task reclaims the SDK's lockout TCB some ticks after flash_safe_execute returns. */
void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
    if (++reclaim_polls >= 3u) {
        task_count = task_count_after_lockout;
    }
}

void flash_range_erase(uint32_t offset, size_t count)
{
    ++pf_test_erase_calls;
    memset(pf_test_flash + offset, 0xff, count);
}

void flash_range_program(uint32_t offset, const uint8_t *data, size_t count)
{
    ++program_calls;
    if (!skip_program) {
        memcpy(pf_test_flash + offset, data, count);
    }
}

int flash_safe_execute(void (*func)(void *), void *param, uint32_t timeout_ms)
{
    (void)timeout_ms;
    ++safe_calls;
    task_count = task_count_after_lockout + 1u;
    if (pf_test_safe_execute_result == PICO_OK) {
        func(param);
        if (during_flash_op != NULL) {
            during_flash_op();
        }
    }
    return pf_test_safe_execute_result;
}

void setUp(void)
{
    memset(pf_test_flash, 0xff, sizeof(pf_test_flash));
    pf_test_erase_calls = program_calls = safe_calls = 0u;
    pf_test_safe_execute_result = PICO_OK;
    skip_program = false;
    scheduler_state = taskSCHEDULER_RUNNING;
    task_count = task_count_after_lockout = 5u;
    reclaim_polls = 0u;
    during_flash_op = NULL;
    pf_test_in_isr = 0;
}

void tearDown(void) {}

void test_reserved_sector_is_the_last_sector_and_aligned(void)
{
    TEST_ASSERT_EQUAL_UINT32(PICO_FLASH_SIZE_BYTES - 4096u, PF_PLATFORM_BOOT_CONFIG_FLASH_OFFSET);
    TEST_ASSERT_EQUAL_UINT32(0u, PF_PLATFORM_BOOT_CONFIG_FLASH_OFFSET % FLASH_SECTOR_SIZE);
}

static const uint8_t record_a[16] = {'P', 'F', 'C', 'F', 1, 0, 16, 0, 200, 0, 50, 25, 1, 2, 3, 4};
static const uint8_t record_b[16] = {'P', 'F', 'C', 'F', 1, 0, 16, 0, 150, 0, 25, 50, 5, 6, 7, 8};

void test_erased_flash_reads_as_erased_bytes(void)
{
    uint8_t data[16] = {0};
    const uint8_t erased[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    TEST_ASSERT_TRUE(pf_platform_boot_config_read(data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(erased, data, sizeof(data));
}

void test_write_then_read_round_trips_and_only_touches_the_reserved_sector(void)
{
    uint8_t data[16];
    pf_test_flash[PF_PLATFORM_BOOT_CONFIG_FLASH_OFFSET - 1u] = 0x5au;
    TEST_ASSERT_TRUE(pf_platform_boot_config_write(record_a, sizeof(record_a)));
    TEST_ASSERT_TRUE(pf_platform_boot_config_read(data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(record_a, data, sizeof(data));
    TEST_ASSERT_EQUAL_UINT8(0x5au, pf_test_flash[PF_PLATFORM_BOOT_CONFIG_FLASH_OFFSET - 1u]);
    TEST_ASSERT_EQUAL_UINT(1u, pf_test_erase_calls);
    TEST_ASSERT_EQUAL_UINT(1u, program_calls);
}

void test_unchanged_write_does_not_erase_again(void)
{
    TEST_ASSERT_TRUE(pf_platform_boot_config_write(record_a, sizeof(record_a)));
    TEST_ASSERT_TRUE(pf_platform_boot_config_write(record_a, sizeof(record_a)));
    TEST_ASSERT_EQUAL_UINT(1u, safe_calls);
}

void test_invalid_buffers_never_reach_flash(void)
{
    uint8_t large[FLASH_PAGE_SIZE + 1u] = {0};
    TEST_ASSERT_FALSE(pf_platform_boot_config_write(NULL, 16u));
    TEST_ASSERT_FALSE(pf_platform_boot_config_write(record_a, 0u));
    TEST_ASSERT_FALSE(pf_platform_boot_config_write(large, sizeof(large)));
    TEST_ASSERT_EQUAL_UINT(0u, safe_calls);
}

void test_unsafe_flash_state_fails_without_writing_and_write_can_retry(void)
{
    pf_test_safe_execute_result = -1;
    TEST_ASSERT_FALSE(pf_platform_boot_config_write(record_a, sizeof(record_a)));
    TEST_ASSERT_EQUAL_UINT(0u, pf_test_erase_calls);
    pf_test_safe_execute_result = PICO_OK;
    TEST_ASSERT_TRUE(pf_platform_boot_config_write(record_a, sizeof(record_a)));
}

void test_failed_program_is_reported_via_readback(void)
{
    skip_program = true;
    TEST_ASSERT_FALSE(pf_platform_boot_config_write(record_a, sizeof(record_a)));
}

static void reenter_during_flash_op(void)
{
    uint8_t data[16];
    TEST_ASSERT_FALSE(pf_platform_boot_config_write(record_b, sizeof(record_b)));
    TEST_ASSERT_FALSE(pf_platform_boot_config_read(data, sizeof(data)));
}

void test_read_and_write_cannot_overlap_a_write_in_progress(void)
{
    uint8_t data[16];
    during_flash_op = reenter_during_flash_op;
    TEST_ASSERT_TRUE(pf_platform_boot_config_write(record_a, sizeof(record_a)));
    TEST_ASSERT_EQUAL_UINT(1u, safe_calls);
    during_flash_op = NULL;
    TEST_ASSERT_TRUE(pf_platform_boot_config_read(data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(record_a, data, sizeof(data));
}

void test_write_and_runtime_read_reject_isr_and_non_running_scheduler(void)
{
    uint8_t data[16];
    BaseType_t states[] = {taskSCHEDULER_NOT_STARTED, taskSCHEDULER_SUSPENDED};
    for (unsigned i = 0u; i < 2u; ++i) {
        scheduler_state = states[i];
        TEST_ASSERT_FALSE(pf_platform_boot_config_write(record_a, sizeof(record_a)));
        TEST_ASSERT_FALSE(pf_platform_boot_config_read(data, sizeof(data)));
    }
    scheduler_state = taskSCHEDULER_RUNNING;
    pf_test_in_isr = 1;
    TEST_ASSERT_FALSE(pf_platform_boot_config_write(record_a, sizeof(record_a)));
    TEST_ASSERT_FALSE(pf_platform_boot_config_read(data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT(0u, safe_calls);
    pf_test_in_isr = 0;
    TEST_ASSERT_TRUE(pf_platform_boot_config_write(record_a, sizeof(record_a)));
}

void test_early_read_works_only_before_the_scheduler_starts(void)
{
    uint8_t data[16];
    TEST_ASSERT_TRUE(pf_platform_boot_config_write(record_a, sizeof(record_a)));
    TEST_ASSERT_FALSE(pf_platform_boot_config_read_early(data, sizeof(data)));
    scheduler_state = taskSCHEDULER_NOT_STARTED;
    TEST_ASSERT_TRUE(pf_platform_boot_config_read_early(data, sizeof(data)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(record_a, data, sizeof(data));
}

void test_write_waits_for_the_lockout_task_and_refuses_while_it_is_unreclaimed(void)
{
    TEST_ASSERT_TRUE(pf_platform_boot_config_write(record_a, sizeof(record_a)));
    TEST_ASSERT_EQUAL_UINT(3u, reclaim_polls);
    TEST_ASSERT_EQUAL_UINT(5u, task_count);

    /* The idle task never reclaims the TCB: the write itself is verified, later writes are refused. */
    reclaim_polls = 0u;
    task_count_after_lockout = 99u;
    task_count = 5u;
    TEST_ASSERT_TRUE(pf_platform_boot_config_write(record_b, sizeof(record_b)));
    TEST_ASSERT_FALSE(pf_platform_boot_config_write(record_a, sizeof(record_a)));
    TEST_ASSERT_EQUAL_UINT(2u, safe_calls);
}
