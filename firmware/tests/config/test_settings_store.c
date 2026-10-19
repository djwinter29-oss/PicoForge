#include "unity.h"

#include <string.h>

TEST_INCLUDE_PATH("firmware/tests/support/config")

#include "FreeRTOS.h"
#include "task.h"
#include "config/settings.h"
#include "config/settings_store.h"

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
    TEST_ASSERT_EQUAL_UINT32(PICO_FLASH_SIZE_BYTES - 4096u, PF_SETTINGS_FLASH_OFFSET);
    TEST_ASSERT_EQUAL_UINT32(0u, PF_SETTINGS_FLASH_OFFSET % FLASH_SECTOR_SIZE);
}

void test_erased_flash_loads_complete_defaults(void)
{
    pf_settings_t settings = {150u, 25u, 25u};
    TEST_ASSERT_FALSE(pf_settings_load(&settings));
    TEST_ASSERT_EQUAL_UINT16(0u, settings.system_clock_mhz);
    TEST_ASSERT_EQUAL_UINT8(0u, settings.sdio_bus_mhz);
    TEST_ASSERT_EQUAL_UINT8(0u, settings.spi_bus_mhz);
}

void test_save_then_load_round_trips_and_only_touches_the_reserved_sector(void)
{
    pf_settings_t saved = {.system_clock_mhz = 250u, .sdio_bus_mhz = 25u, .spi_bus_mhz = 50u}, loaded;
    pf_test_flash[PF_SETTINGS_FLASH_OFFSET - 1u] = 0x5au;
    TEST_ASSERT_TRUE(pf_settings_save(&saved));
    TEST_ASSERT_TRUE(pf_settings_load(&loaded));
    TEST_ASSERT_EQUAL_MEMORY(&saved, &loaded, sizeof(saved));
    TEST_ASSERT_EQUAL_UINT8(0x5au, pf_test_flash[PF_SETTINGS_FLASH_OFFSET - 1u]);
    TEST_ASSERT_EQUAL_UINT(1u, pf_test_erase_calls);
    TEST_ASSERT_EQUAL_UINT(1u, program_calls);
}

void test_unchanged_save_does_not_erase_again(void)
{
    pf_settings_t settings = {.spi_bus_mhz = 25u};
    TEST_ASSERT_TRUE(pf_settings_save(&settings));
    TEST_ASSERT_TRUE(pf_settings_save(&settings));
    TEST_ASSERT_EQUAL_UINT(1u, safe_calls);
}

void test_invalid_settings_never_reach_flash(void)
{
    pf_settings_t invalid = {.system_clock_mhz = 100u};
    TEST_ASSERT_FALSE(pf_settings_save(&invalid));
    TEST_ASSERT_FALSE(pf_settings_save(NULL));
    TEST_ASSERT_EQUAL_UINT(0u, safe_calls);
}

void test_unsafe_flash_state_fails_without_writing_and_save_can_retry(void)
{
    pf_settings_t settings = {.sdio_bus_mhz = 50u};
    pf_test_safe_execute_result = -1;
    TEST_ASSERT_FALSE(pf_settings_save(&settings));
    TEST_ASSERT_EQUAL_UINT(0u, pf_test_erase_calls);
    pf_test_safe_execute_result = PICO_OK;
    TEST_ASSERT_TRUE(pf_settings_save(&settings));
}

void test_failed_program_is_reported_via_readback(void)
{
    pf_settings_t settings = {.sdio_bus_mhz = 50u};
    skip_program = true;
    TEST_ASSERT_FALSE(pf_settings_save(&settings));
}

void test_failed_erase_is_reported_and_a_retry_succeeds(void)
{
    pf_settings_t settings = {.spi_bus_mhz = 25u}, loaded;
    pf_test_safe_execute_result = -7;
    TEST_ASSERT_FALSE(pf_settings_save(&settings));
    TEST_ASSERT_FALSE(pf_settings_load(&loaded));
    pf_test_safe_execute_result = PICO_OK;
    TEST_ASSERT_TRUE(pf_settings_save(&settings));
    TEST_ASSERT_TRUE(pf_settings_load(&loaded));
}

static void reenter_during_flash_op(void)
{
    static pf_settings_t other = {.spi_bus_mhz = 50u};
    pf_settings_t loaded = {.spi_bus_mhz = 25u};
    TEST_ASSERT_FALSE(pf_settings_save(&other));
    TEST_ASSERT_FALSE(pf_settings_load(&loaded));
    TEST_ASSERT_EQUAL_UINT8(0u, loaded.spi_bus_mhz);
}

void test_load_and_save_cannot_overlap_a_save_in_progress(void)
{
    pf_settings_t settings = {.sdio_bus_mhz = 25u}, loaded;
    during_flash_op = reenter_during_flash_op;
    TEST_ASSERT_TRUE(pf_settings_save(&settings));
    TEST_ASSERT_EQUAL_UINT(1u, safe_calls);
    during_flash_op = NULL;
    TEST_ASSERT_TRUE(pf_settings_load(&loaded));
    TEST_ASSERT_EQUAL_UINT8(25u, loaded.sdio_bus_mhz);
}

void test_save_and_runtime_load_reject_isr_and_non_running_scheduler(void)
{
    pf_settings_t settings = {.spi_bus_mhz = 25u}, loaded = {.spi_bus_mhz = 25u};
    BaseType_t states[] = {taskSCHEDULER_NOT_STARTED, taskSCHEDULER_SUSPENDED};
    for (unsigned i = 0u; i < 2u; ++i) {
        scheduler_state = states[i];
        TEST_ASSERT_FALSE(pf_settings_save(&settings));
        TEST_ASSERT_FALSE(pf_settings_load(&loaded));
    }
    scheduler_state = taskSCHEDULER_RUNNING;
    pf_test_in_isr = 1;
    TEST_ASSERT_FALSE(pf_settings_save(&settings));
    TEST_ASSERT_FALSE(pf_settings_load(&loaded));
    TEST_ASSERT_EQUAL_UINT(0u, safe_calls);
    pf_test_in_isr = 0;
    TEST_ASSERT_TRUE(pf_settings_save(&settings));
}

void test_early_load_reads_only_before_the_scheduler_starts(void)
{
    pf_settings_t settings = {.system_clock_mhz = 200u}, loaded;
    TEST_ASSERT_TRUE(pf_settings_save(&settings));
    TEST_ASSERT_FALSE(pf_settings_load_early(&loaded));
    TEST_ASSERT_EQUAL_UINT16(0u, loaded.system_clock_mhz);
    scheduler_state = taskSCHEDULER_NOT_STARTED;
    TEST_ASSERT_TRUE(pf_settings_load_early(&loaded));
    TEST_ASSERT_EQUAL_UINT16(200u, loaded.system_clock_mhz);
}

void test_save_waits_for_the_lockout_task_and_refuses_while_it_is_unreclaimed(void)
{
    pf_settings_t first = {.spi_bus_mhz = 25u}, second = {.spi_bus_mhz = 50u};
    TEST_ASSERT_TRUE(pf_settings_save(&first));
    TEST_ASSERT_EQUAL_UINT(3u, reclaim_polls);
    TEST_ASSERT_EQUAL_UINT(5u, task_count);

    /* The idle task never reclaims the TCB: the save itself is verified, later saves are refused. */
    reclaim_polls = 0u;
    task_count_after_lockout = 99u;
    task_count = 5u;
    TEST_ASSERT_TRUE(pf_settings_save(&second));
    TEST_ASSERT_FALSE(pf_settings_save(&first));
    TEST_ASSERT_EQUAL_UINT(2u, safe_calls);
}
