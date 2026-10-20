/**
 * @file settings_store.c
 * @brief Flash persistence for pf_settings_t in one reserved 4 KiB sector (single copy, no wear levelling).
 *
 * ponytail: one sector, one copy, no wear levelling. Fine for infrequent user configuration (a few saves per
 * session at most, far below the flash erase endurance). A power loss mid-save leaves an erased or partial record
 * that fails CRC and loads as Defaults, so settings can be lost but never half-applied. Upgrade path: a
 * dual-sector journal (sequence number + CRC, write the other sector, then retire the old one) if atomic
 * retention across power loss or frequent writes are ever needed.
 */
#include "drivers/persistent_memory/settings_store.h"

#include <string.h>

#include "FreeRTOS.h"
#include "pico/flash.h"
#include "pico/mutex.h"
#include "task.h"

#define LOCK_TIMEOUT_MS 1000u
#define SAVE_TIMEOUT_MS 1000u
#define LOCKOUT_RECLAIM_TIMEOUT_MS 1000u

_Static_assert(PF_SETTINGS_FLASH_OFFSET % FLASH_SECTOR_SIZE == 0u, "settings sector must be sector aligned");
_Static_assert(PF_SETTINGS_RECORD_SIZE <= FLASH_PAGE_SIZE, "record must fit one flash page");

/** Erase/program request; lives on the caller's stack for the duration of flash_safe_execute. */
typedef struct {
    uint8_t page[FLASH_PAGE_SIZE]; /**< Record padded with 0xFF. */
} write_request_t;

/** Serializes load, save and the flash-safe zone. */
auto_init_mutex(store_mutex);
/** Task count before the last flash_safe_execute; valid while lockout_pending. Guarded by store_mutex. */
static UBaseType_t tasks_before_lockout;
/** True while the SDK's static lockout task may not have been reclaimed yet. Guarded by store_mutex. */
static bool lockout_pending;

static const uint8_t *stored_record(void)
{
    return (const uint8_t *)(XIP_BASE + PF_SETTINGS_FLASH_OFFSET);
}

static bool task_context(void)
{
    return !xPortIsInsideInterrupt() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
}

/** Runs with interrupts disabled and the other core held out of flash. */
static void erase_and_program(void *param)
{
    const write_request_t *request = param;
    flash_range_erase(PF_SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(PF_SETTINGS_FLASH_OFFSET, request->page, FLASH_PAGE_SIZE);
}

/** Wait for the idle task to reclaim the SDK's static lockout TCB (the task count returns to its old value). */
static bool lockout_reclaimed(uint32_t timeout_ms)
{
    while (uxTaskGetNumberOfTasks() != tasks_before_lockout) {
        if (timeout_ms-- == 0u) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return true;
}

bool pf_settings_load_early(pf_settings_t *settings)
{
    if (settings == NULL) {
        return false;
    }
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        pf_settings_defaults(settings);
        return false;
    }
    return pf_settings_decode(stored_record(), PF_SETTINGS_RECORD_SIZE, settings);
}

bool pf_settings_load(pf_settings_t *settings)
{
    bool loaded;
    if (settings == NULL) {
        return false;
    }
    if (!task_context() || !mutex_enter_timeout_ms(&store_mutex, LOCK_TIMEOUT_MS)) {
        pf_settings_defaults(settings);
        return false;
    }
    loaded = pf_settings_decode(stored_record(), PF_SETTINGS_RECORD_SIZE, settings);
    mutex_exit(&store_mutex);
    return loaded;
}

bool pf_settings_save(const pf_settings_t *settings)
{
    write_request_t request;
    bool saved = false;

    memset(request.page, 0xff, sizeof(request.page));
    if (!task_context() || !pf_settings_encode(settings, request.page) ||
        !mutex_enter_timeout_ms(&store_mutex, LOCK_TIMEOUT_MS)) {
        return false;
    }
    if (lockout_pending) {
        lockout_pending = !lockout_reclaimed(0u);
    }
    if (lockout_pending) {
        /* The SDK's static lockout TCB may still be in use; refuse rather than reuse it. */
    } else if (memcmp(stored_record(), request.page, PF_SETTINGS_RECORD_SIZE) == 0) {
        saved = true;
    } else {
        tasks_before_lockout = uxTaskGetNumberOfTasks();
        int rc = flash_safe_execute(erase_and_program, &request, SAVE_TIMEOUT_MS);
        lockout_pending = !lockout_reclaimed(LOCKOUT_RECLAIM_TIMEOUT_MS);
        saved = rc == PICO_OK && memcmp(stored_record(), request.page, PF_SETTINGS_RECORD_SIZE) == 0;
    }
    mutex_exit(&store_mutex);
    return saved;
}