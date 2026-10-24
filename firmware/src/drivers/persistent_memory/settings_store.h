#ifndef PF_PERSISTENT_MEMORY_SETTINGS_STORE_H
#define PF_PERSISTENT_MEMORY_SETTINGS_STORE_H

#include "drivers/persistent_memory/settings.h"
#include "hardware/flash.h"

/** Flash offset of the reserved settings sector: the last sector of the board's flash (PICO_FLASH_SIZE_BYTES). */
#define PF_SETTINGS_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

/**
 * @brief Read the persisted settings before the FreeRTOS scheduler starts (early boot, runtime-init hooks, main()).
 *
 * Lock-free and read-only: nothing can erase or program flash yet. Once the scheduler has started this refuses
 * to read and returns defaults, so a late caller cannot race a save; use pf_settings_load() instead.
 * @param settings Always filled; complete defaults unless a valid record was loaded.
 * @return true if a valid record was loaded.
 */
bool pf_settings_load_early(pf_settings_t *settings);

/**
 * @brief Read the persisted settings from the designated settings-owner task.
 *
 * Runtime settings access is owned by the USB task, which also performs saves; callers must not access this API
 * concurrently from other tasks. Returns false with complete defaults during a save, from an ISR, before the
 * scheduler runs (use pf_settings_load_early()), or when the scheduler is suspended.
 * @param settings Always filled; complete defaults unless a valid record was loaded.
 * @return true if a valid record was loaded.
 */
bool pf_settings_load(pf_settings_t *settings);

/**
 * @brief Persist @p settings: erase and program the reserved sector through flash_safe_execute (about 50 ms with
 * interrupts disabled).
 *
 * Call only from the designated settings-owner task with the scheduler running; ISRs, pre-scheduler and
 * suspended-scheduler callers are rejected. This is the sole project flash_safe_execute() caller. The SDK's
 * FreeRTOS flash lockout reuses one static task control block, so this function waits for that task to be reclaimed
 * before it returns and refuses further saves if it never is. Do not create or delete tasks while a save is in
 * progress.
 * @return false, without touching flash, for invalid settings, an unsafe context or a busy mutex; true only after
 * the stored record reads back identical. An unchanged record is not rewritten. A re-entrant save is rejected.
 */
bool pf_settings_save(const pf_settings_t *settings);

#endif