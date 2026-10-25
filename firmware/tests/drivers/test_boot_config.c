#include "unity.h"

#include <stdbool.h>
#include <string.h>

TEST_INCLUDE_PATH("firmware/tests/support/config")

#include "FreeRTOS.h"
#include "hardware/flash.h"
#include "pico/flash.h"
#include "task.h"

uint8_t pf_test_flash[PICO_FLASH_SIZE_BYTES];
unsigned pf_test_erase_calls;
int pf_test_safe_execute_result;
static unsigned program_calls;
static unsigned safe_calls;
static bool skip_program;
static bool fail_after_program;
static bool fail_before_task;
static size_t program_limit;
static TickType_t tick_count;
static TickType_t delay_ticks;
static BaseType_t scheduler_state;
static UBaseType_t task_count;
static UBaseType_t task_count_after_lockout;
static unsigned reclaim_polls;
static void (*during_flash_op)(void);
int pf_test_in_isr;

BaseType_t xPortIsInsideInterrupt(void) { return pf_test_in_isr; }
BaseType_t xTaskGetSchedulerState(void) { return scheduler_state; }
UBaseType_t uxTaskGetNumberOfTasks(void) { return task_count; }
TickType_t xTaskGetTickCount(void) { return tick_count; }

/* The idle task reclaims the SDK's static lockout TCB some ticks after flash_safe_execute returns. */
void vTaskDelay(TickType_t ticks)
{
    TEST_ASSERT_EQUAL_UINT32(1u, ticks);
    tick_count += delay_ticks;
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
        memcpy(pf_test_flash + offset, data, count < program_limit ? count : program_limit);
    }
}

int flash_safe_execute(void (*func)(void *), void *param, uint32_t timeout_ms)
{
    (void)timeout_ms;
    ++safe_calls;
    if (fail_before_task) {
        return pf_test_safe_execute_result;
    }
    task_count = task_count_after_lockout + 1u;
    if (pf_test_safe_execute_result == PICO_OK || fail_after_program) {
        func(param);
        if (during_flash_op != NULL) {
            during_flash_op();
        }
    }
    return pf_test_safe_execute_result;
}

#define BOOT_CONFIG_SOURCE "drivers/boot_config.c"
#include BOOT_CONFIG_SOURCE

static uint8_t *stored_bytes(void)
{
    return pf_test_flash + BOOT_CONFIG_FLASH_OFFSET;
}

static void reseal(void)
{
    record_t *stored = (record_t *)stored_bytes();
    stored->crc = crc32(stored, offsetof(record_t, crc));
}

static void store(uint8_t sysclk_profile, uint8_t sdcard_profile)
{
    const uint8_t header[] = {'P', 'F', 'C', 'F', 1, 0, 16, 0};
    uint8_t *bytes = stored_bytes();
    memcpy(bytes, header, sizeof(header));
    record_t *stored = (record_t *)bytes;
    stored->sysclk_profile = sysclk_profile;
    stored->sdcard_profile = sdcard_profile;
    stored->reserved = 0u;
    reseal();
}

static void reboot(void)
{
    loaded = false;
    config = (record_t){0};
    save_in_progress = false;
    lockout_pending = false;
    tasks_before_lockout = 0u;
    scheduler_state = taskSCHEDULER_NOT_STARTED;
    task_count = task_count_after_lockout = 5u;
    reclaim_polls = 0u;
}

void setUp(void)
{
    memset(pf_test_flash, 0xff, sizeof(pf_test_flash));
    pf_test_erase_calls = 0u;
    program_calls = 0u;
    safe_calls = 0u;
    skip_program = false;
    fail_after_program = false;
    fail_before_task = false;
    program_limit = FLASH_PAGE_SIZE;
    tick_count = 0u;
    delay_ticks = 1u;
    pf_test_safe_execute_result = PICO_OK;
    task_count_after_lockout = 5u;
    during_flash_op = NULL;
    pf_test_in_isr = 0;
    reboot();
}

void tearDown(void) {}

void test_profile_conversions_report_mhz_and_fall_back_for_unknown_values(void)
{
    TEST_ASSERT_EQUAL_UINT16(200u, pf_sysclk_profile_mhz(PF_SYSCLK_200MHZ));
    TEST_ASSERT_EQUAL_UINT16(0u, pf_sysclk_profile_mhz((pf_sysclk_profile_t)4));
    TEST_ASSERT_EQUAL_UINT8(25u, pf_storage_speed_mhz(PF_STORAGE_SPEED_DEFAULT));
    TEST_ASSERT_EQUAL_UINT8(50u, pf_storage_speed_mhz(PF_STORAGE_SPEED_HIGH));
    TEST_ASSERT_EQUAL_UINT8(25u, pf_storage_speed_mhz((pf_storage_speed_t)2));
}

void test_crc32_matches_the_standard_check_value(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xcbf43926u, crc32((const uint8_t *)"123456789", 9u));
}

void test_configured_mhz_getters_load_defaults_saved_profiles_and_ram_updates(void)
{
    TEST_ASSERT_EQUAL_UINT16(0u, pf_get_sysclk_mhz());
    TEST_ASSERT_EQUAL_UINT8(25u, pf_get_storage_speed_mhz());
    store(PF_SYSCLK_200MHZ, PF_STORAGE_SPEED_HIGH);
    reboot();
    TEST_ASSERT_EQUAL_UINT8(50u, pf_get_storage_speed_mhz());
    TEST_ASSERT_EQUAL_UINT16(200u, pf_get_sysclk_mhz());
    TEST_ASSERT_TRUE(pf_boot_config_set_sysclk(PF_SYSCLK_150MHZ));
    TEST_ASSERT_TRUE(pf_boot_config_set_storage_speed(PF_STORAGE_SPEED_DEFAULT));
    TEST_ASSERT_EQUAL_UINT16(150u, pf_get_sysclk_mhz());
    TEST_ASSERT_EQUAL_UINT8(25u, pf_get_storage_speed_mhz());
    TEST_ASSERT_EQUAL_UINT(0u, safe_calls);
}

void test_erased_storage_gives_sdk_clock_and_default_sd_speed(void)
{
    TEST_ASSERT_EQUAL(PF_SYSCLK_DEFAULT, pf_boot_config_sysclk());
    TEST_ASSERT_EQUAL(PF_STORAGE_SPEED_DEFAULT, pf_boot_config_storage_speed());
}

void test_early_load_reads_the_saved_record(void)
{
    store(PF_SYSCLK_200MHZ, PF_STORAGE_SPEED_HIGH);
    TEST_ASSERT_EQUAL(PF_SYSCLK_200MHZ, pf_boot_config_sysclk());
    TEST_ASSERT_EQUAL(PF_STORAGE_SPEED_HIGH, pf_boot_config_storage_speed());
}

void test_runtime_first_access_loads_the_saved_record(void)
{
    store(PF_SYSCLK_250MHZ, PF_STORAGE_SPEED_HIGH);
    scheduler_state = taskSCHEDULER_RUNNING;
    TEST_ASSERT_EQUAL(PF_SYSCLK_250MHZ, pf_boot_config_sysclk());
}

void test_setters_reject_unsupported_values_without_changing_the_configuration(void)
{
    TEST_ASSERT_FALSE(pf_boot_config_set_sysclk((pf_sysclk_profile_t)4));
    TEST_ASSERT_FALSE(pf_boot_config_set_sysclk((pf_sysclk_profile_t)-1));
    TEST_ASSERT_FALSE(pf_boot_config_set_storage_speed((pf_storage_speed_t)2));
    TEST_ASSERT_EQUAL(PF_SYSCLK_DEFAULT, pf_boot_config_sysclk());
    TEST_ASSERT_EQUAL(PF_STORAGE_SPEED_DEFAULT, pf_boot_config_storage_speed());
}

void test_save_writes_the_v1_profile_layout_and_survives_reboot(void)
{
    for (unsigned i = 0u; i < 4u; ++i) {
        for (unsigned speed = 0u; speed < 2u; ++speed) {
            reboot();
            scheduler_state = taskSCHEDULER_RUNNING;
            TEST_ASSERT_TRUE(pf_boot_config_set_sysclk((pf_sysclk_profile_t)i));
            TEST_ASSERT_TRUE(pf_boot_config_set_storage_speed((pf_storage_speed_t)speed));
            TEST_ASSERT_TRUE(pf_boot_config_save());
            TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){'P', 'F', 'C', 'F', 1, 0, 16, 0,
                                                      (uint8_t)i, (uint8_t)speed, 0u, 0u}),
                                         stored_bytes(), 12);
            reboot();
            TEST_ASSERT_EQUAL(i, pf_boot_config_sysclk());
            TEST_ASSERT_EQUAL(speed, pf_boot_config_storage_speed());
        }
    }
}

void test_failed_write_is_reported(void)
{
    pf_test_safe_execute_result = -1;
    TEST_ASSERT_TRUE(pf_boot_config_set_sysclk(PF_SYSCLK_150MHZ));
    scheduler_state = taskSCHEDULER_RUNNING;
    TEST_ASSERT_FALSE(pf_boot_config_save());
    TEST_ASSERT_EQUAL_UINT(1u, safe_calls);
}

void test_exit_failure_reports_unconfirmed_even_when_new_record_was_persisted(void)
{
    scheduler_state = taskSCHEDULER_RUNNING;
    TEST_ASSERT_TRUE(pf_boot_config_set_sysclk(PF_SYSCLK_200MHZ));
    fail_after_program = true;
    pf_test_safe_execute_result = -1;
    TEST_ASSERT_FALSE(pf_boot_config_save());
    TEST_ASSERT_EQUAL_UINT(1u, program_calls);
    reboot();
    TEST_ASSERT_EQUAL(PF_SYSCLK_200MHZ, pf_boot_config_sysclk());
}

void test_corrupt_or_unknown_records_give_defaults(void)
{
    const struct { unsigned offset; uint8_t value; bool reseal; } cases[] = {
        {0u, 'X', true}, {4u, 2u, true}, {6u, 15u, true}, {8u, 4u, true}, {9u, 2u, true},
        {10u, 1u, true}, {11u, 1u, true}, {8u, 1u, false}, {15u, 0u, false},
    };
    for (unsigned i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        store(PF_SYSCLK_200MHZ, PF_STORAGE_SPEED_HIGH);
        stored_bytes()[cases[i].offset] = cases[i].value;
        if (cases[i].reseal) {
            reseal();
        }
        reboot();
        TEST_ASSERT_EQUAL(PF_SYSCLK_DEFAULT, pf_boot_config_sysclk());
        TEST_ASSERT_EQUAL(PF_STORAGE_SPEED_DEFAULT, pf_boot_config_storage_speed());
    }
}

void test_reserved_sector_is_the_last_sector_and_aligned(void)
{
    TEST_ASSERT_EQUAL_UINT32(PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE, BOOT_CONFIG_FLASH_OFFSET);
    TEST_ASSERT_EQUAL_UINT32(0u, BOOT_CONFIG_FLASH_OFFSET % FLASH_SECTOR_SIZE);
}

void test_save_only_touches_the_reserved_sector_and_pads_the_page(void)
{
    scheduler_state = taskSCHEDULER_RUNNING;
    pf_test_flash[BOOT_CONFIG_FLASH_OFFSET - 1u] = 0x5au;
    TEST_ASSERT_TRUE(pf_boot_config_set_sysclk(PF_SYSCLK_200MHZ));
    TEST_ASSERT_TRUE(pf_boot_config_save());
    TEST_ASSERT_TRUE(record_valid((const record_t *)stored_bytes()));
    TEST_ASSERT_EQUAL_UINT8(0x5au, pf_test_flash[BOOT_CONFIG_FLASH_OFFSET - 1u]);
    for (size_t i = sizeof(record_t); i < FLASH_SECTOR_SIZE; ++i) {
        TEST_ASSERT_EQUAL_HEX8(0xffu, stored_bytes()[i]);
    }
    TEST_ASSERT_EQUAL_UINT(1u, pf_test_erase_calls);
    TEST_ASSERT_EQUAL_UINT(1u, program_calls);
}

void test_unchanged_write_does_not_erase_again(void)
{
    scheduler_state = taskSCHEDULER_RUNNING;
    TEST_ASSERT_TRUE(pf_boot_config_save());
    TEST_ASSERT_TRUE(pf_boot_config_save());
    TEST_ASSERT_EQUAL_UINT(1u, safe_calls);
}

void test_unsafe_flash_state_fails_without_writing_and_write_can_retry(void)
{
    scheduler_state = taskSCHEDULER_RUNNING;
    pf_test_safe_execute_result = -1;
    TEST_ASSERT_FALSE(pf_boot_config_save());
    TEST_ASSERT_EQUAL_UINT(0u, pf_test_erase_calls);
    TEST_ASSERT_EQUAL_UINT(0u, program_calls);
    TEST_ASSERT_EQUAL_UINT(3u, reclaim_polls);
    TEST_ASSERT_FALSE(lockout_pending);
    pf_test_safe_execute_result = PICO_OK;
    TEST_ASSERT_TRUE(pf_boot_config_save());
    TEST_ASSERT_EQUAL_UINT(2u, safe_calls);
}

void test_failure_before_task_creation_needs_no_reclamation_and_can_retry(void)
{
    store(PF_SYSCLK_150MHZ, PF_STORAGE_SPEED_HIGH);
    scheduler_state = taskSCHEDULER_RUNNING;
    TEST_ASSERT_TRUE(pf_boot_config_set_sysclk(PF_SYSCLK_200MHZ));
    fail_before_task = true;
    pf_test_safe_execute_result = -1;
    TEST_ASSERT_FALSE(pf_boot_config_save());
    TEST_ASSERT_EQUAL_UINT(0u, pf_test_erase_calls);
    TEST_ASSERT_EQUAL_UINT(0u, program_calls);
    TEST_ASSERT_EQUAL_UINT(0u, reclaim_polls);
    TEST_ASSERT_FALSE(lockout_pending);
    TEST_ASSERT_EQUAL_UINT8(PF_SYSCLK_150MHZ, stored_bytes()[8]);
    fail_before_task = false;
    pf_test_safe_execute_result = PICO_OK;
    TEST_ASSERT_TRUE(pf_boot_config_save());
    TEST_ASSERT_EQUAL_UINT(2u, safe_calls);
    reboot();
    TEST_ASSERT_EQUAL(PF_SYSCLK_200MHZ, pf_boot_config_sysclk());
}

void test_partial_program_defaults_after_reboot_and_successful_save_recovers(void)
{
    for (size_t bytes = 1u; bytes < sizeof(record_t); ++bytes) {
        store(PF_SYSCLK_150MHZ, PF_STORAGE_SPEED_DEFAULT);
        reboot();
        scheduler_state = taskSCHEDULER_RUNNING;
        TEST_ASSERT_TRUE(pf_boot_config_set_sysclk(PF_SYSCLK_200MHZ));
        TEST_ASSERT_TRUE(pf_boot_config_set_storage_speed(PF_STORAGE_SPEED_HIGH));
        program_limit = bytes;
        TEST_ASSERT_FALSE(pf_boot_config_save());
        reboot();
        TEST_ASSERT_EQUAL(PF_SYSCLK_DEFAULT, pf_boot_config_sysclk());
        TEST_ASSERT_EQUAL(PF_STORAGE_SPEED_DEFAULT, pf_boot_config_storage_speed());
        scheduler_state = taskSCHEDULER_RUNNING;
        program_limit = FLASH_PAGE_SIZE;
        TEST_ASSERT_TRUE(pf_boot_config_set_sysclk(PF_SYSCLK_200MHZ));
        TEST_ASSERT_TRUE(pf_boot_config_set_storage_speed(PF_STORAGE_SPEED_HIGH));
        TEST_ASSERT_TRUE(pf_boot_config_save());
        reboot();
        TEST_ASSERT_EQUAL(PF_SYSCLK_200MHZ, pf_boot_config_sysclk());
        TEST_ASSERT_EQUAL(PF_STORAGE_SPEED_HIGH, pf_boot_config_storage_speed());
    }
}

void test_failed_program_is_reported_via_readback(void)
{
    scheduler_state = taskSCHEDULER_RUNNING;
    skip_program = true;
    TEST_ASSERT_FALSE(pf_boot_config_save());
}

static void reenter_during_flash_op(void)
{
    TEST_ASSERT_FALSE(pf_boot_config_save());
    TEST_ASSERT_EQUAL(PF_SYSCLK_200MHZ, pf_boot_config_sysclk());
}

void test_reentrant_save_is_rejected_and_getters_use_ram(void)
{
    scheduler_state = taskSCHEDULER_RUNNING;
    TEST_ASSERT_TRUE(pf_boot_config_set_sysclk(PF_SYSCLK_200MHZ));
    during_flash_op = reenter_during_flash_op;
    TEST_ASSERT_TRUE(pf_boot_config_save());
    TEST_ASSERT_EQUAL_UINT(1u, safe_calls);
}

void test_save_rejects_isr_and_non_running_scheduler(void)
{
    BaseType_t states[] = {taskSCHEDULER_NOT_STARTED, taskSCHEDULER_SUSPENDED};
    for (unsigned i = 0u; i < 2u; ++i) {
        scheduler_state = states[i];
        TEST_ASSERT_FALSE(pf_boot_config_save());
    }
    scheduler_state = taskSCHEDULER_RUNNING;
    pf_test_in_isr = 1;
    TEST_ASSERT_FALSE(pf_boot_config_save());
    TEST_ASSERT_EQUAL_UINT(0u, safe_calls);
    pf_test_in_isr = 0;
    TEST_ASSERT_TRUE(pf_boot_config_save());
}

void test_unsafe_initial_load_can_retry_and_setters_do_not_overwrite_saved_values(void)
{
    store(PF_SYSCLK_200MHZ, PF_STORAGE_SPEED_HIGH);
    for (unsigned i = 0u; i < 3u; ++i) {
        reboot();
        scheduler_state = i == 0u ? taskSCHEDULER_SUSPENDED : taskSCHEDULER_RUNNING;
        pf_test_in_isr = i == 1u;
        save_in_progress = i == 2u;
        TEST_ASSERT_EQUAL(PF_SYSCLK_DEFAULT, pf_boot_config_sysclk());
        TEST_ASSERT_FALSE(pf_boot_config_set_sysclk(PF_SYSCLK_150MHZ));
        TEST_ASSERT_FALSE(pf_boot_config_set_storage_speed(PF_STORAGE_SPEED_DEFAULT));
        pf_test_in_isr = 0;
        save_in_progress = false;
        scheduler_state = taskSCHEDULER_RUNNING;
        TEST_ASSERT_EQUAL(PF_SYSCLK_200MHZ, pf_boot_config_sysclk());
        TEST_ASSERT_EQUAL(PF_STORAGE_SPEED_HIGH, pf_boot_config_storage_speed());
    }
    TEST_ASSERT_EQUAL_UINT(0u, safe_calls);
}

void test_lockout_timeout_uses_elapsed_ticks_across_wraparound(void)
{
    tasks_before_lockout = 5u;
    task_count = task_count_after_lockout = 6u;
    tick_count = UINT32_MAX - 99u;
    delay_ticks = 400u;
    TEST_ASSERT_FALSE(lockout_reclaimed(1000u));
    TEST_ASSERT_EQUAL_UINT(3u, reclaim_polls);
    TEST_ASSERT_EQUAL_UINT32(1100u, tick_count);
}

void test_write_waits_for_the_lockout_task_and_refuses_while_it_is_unreclaimed(void)
{
    scheduler_state = taskSCHEDULER_RUNNING;
    TEST_ASSERT_TRUE(pf_boot_config_save());
    TEST_ASSERT_EQUAL_UINT(3u, reclaim_polls);
    TEST_ASSERT_EQUAL_UINT(5u, task_count);

    /* The idle task never reclaims the TCB: the write itself is verified, later writes are refused. */
    reclaim_polls = 0u;
    task_count_after_lockout = 99u;
    task_count = 5u;
    TEST_ASSERT_TRUE(pf_boot_config_set_sysclk(PF_SYSCLK_200MHZ));
    TEST_ASSERT_TRUE(pf_boot_config_save());
    TEST_ASSERT_TRUE(pf_boot_config_set_sysclk(PF_SYSCLK_150MHZ));
    TEST_ASSERT_FALSE(pf_boot_config_save());
    TEST_ASSERT_EQUAL_UINT(2u, safe_calls);
    task_count = tasks_before_lockout;
    TEST_ASSERT_TRUE(pf_boot_config_save());
    TEST_ASSERT_EQUAL_UINT(3u, safe_calls);
}
