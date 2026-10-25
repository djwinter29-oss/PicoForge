/**
 * @file boot_config_store.c
 * @brief RP2 flash storage for opaque boot-configuration records.
 *
 * ponytail: one sector, one copy, no wear levelling. Fine for infrequent user configuration (a few saves per
 * session at most, far below flash erase endurance). Power loss mid-save can lose the record, but the config
 * codec rejects partial data and boot falls back to defaults. A dual-sector journal is the upgrade if atomic
 * retention or frequent writes become requirements.
 */
#include "platform/boot_config_store.h"

#include <string.h>

#include "FreeRTOS.h"
#include "pico/flash.h"
#include "task.h"

#define SAVE_TIMEOUT_MS 1000u
#define LOCKOUT_RECLAIM_TIMEOUT_MS 1000u

_Static_assert(PF_PLATFORM_BOOT_CONFIG_FLASH_OFFSET % FLASH_SECTOR_SIZE == 0u, "config sector must be aligned");

/** Erase/program request; lives on the caller's stack for the duration of flash_safe_execute. */
typedef struct {
    uint8_t page[FLASH_PAGE_SIZE]; /**< Opaque data padded with 0xFF. */
} write_request_t;

/** Prevents access while flash_safe_execute is active. */
static bool save_in_progress;
/** Task count before flash_safe_execute; valid while lockout_pending. */
static UBaseType_t tasks_before_lockout;
/** True while the SDK's static lockout task may not have been reclaimed yet. */
static bool lockout_pending;

static const uint8_t *stored_data(void)
{
    return (const uint8_t *)(XIP_BASE + PF_PLATFORM_BOOT_CONFIG_FLASH_OFFSET);
}

static bool task_context(void)
{
    return !xPortIsInsideInterrupt() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
}

/** Runs with interrupts disabled and the other core held out of flash. */
static void erase_and_program(void *param)
{
    const write_request_t *request = param;
    flash_range_erase(PF_PLATFORM_BOOT_CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(PF_PLATFORM_BOOT_CONFIG_FLASH_OFFSET, request->page, FLASH_PAGE_SIZE);
}

/** Wait for the idle task to reclaim the SDK's static lockout TCB. */
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

bool pf_platform_boot_config_read_early(uint8_t *data, size_t size)
{
    if (data == NULL || size == 0u || size > FLASH_PAGE_SIZE ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        return false;
    }
    memcpy(data, stored_data(), size);
    return true;
}

bool pf_platform_boot_config_read(uint8_t *data, size_t size)
{
    if (data == NULL || size == 0u || size > FLASH_PAGE_SIZE || !task_context() || save_in_progress) {
        return false;
    }
    memcpy(data, stored_data(), size);
    return true;
}

bool pf_platform_boot_config_write(const uint8_t *data, size_t size)
{
    write_request_t request;
    bool saved = false;

    if (data == NULL || size == 0u || size > FLASH_PAGE_SIZE || !task_context() || save_in_progress) {
        return false;
    }
    memset(request.page, 0xff, sizeof(request.page));
    memcpy(request.page, data, size);
    save_in_progress = true;
    if (lockout_pending) {
        lockout_pending = !lockout_reclaimed(0u);
    }
    if (lockout_pending) {
        /* The SDK's static lockout TCB may still be in use; refuse rather than reuse it. */
    } else if (memcmp(stored_data(), request.page, size) == 0) {
        saved = true;
    } else {
        tasks_before_lockout = uxTaskGetNumberOfTasks();
        int rc = flash_safe_execute(erase_and_program, &request, SAVE_TIMEOUT_MS);
        lockout_pending = !lockout_reclaimed(LOCKOUT_RECLAIM_TIMEOUT_MS);
        saved = rc == PICO_OK && memcmp(stored_data(), request.page, size) == 0;
    }
    save_in_progress = false;
    return saved;
}
