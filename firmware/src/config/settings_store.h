#ifndef PF_CONFIG_SETTINGS_STORE_H
#define PF_CONFIG_SETTINGS_STORE_H

#include "config/settings.h"
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
 * @brief Read the persisted settings from a running FreeRTOS task, serialized against pf_settings_save().
 *
 * Waits up to one second for an in-progress save. Returns false with complete defaults when called from an ISR or
 * before the scheduler runs (use pf_settings_load_early()), when the scheduler is suspended, or when the wait
 * times out.
 * @param settings Always filled; complete defaults unless a valid record was loaded.
 * @return true if a valid record was loaded.
 */
bool pf_settings_load(pf_settings_t *settings);

/**
 * @brief Persist @p settings: erase and program the reserved sector through flash_safe_execute (about 50 ms with
 * interrupts disabled).
 *
 * Call only from a FreeRTOS task with the scheduler running; ISRs, pre-scheduler and suspended-scheduler callers
 * are rejected. Saves and loads are serialized by one mutex, so a concurrent or re-entrant call fails instead of
 * overlapping. The SDK's FreeRTOS flash lockout reuses one static task control block, so this function waits for
 * that task to be reclaimed before it returns and refuses further saves if it never is: it must remain the only
 * flash_safe_execute() caller, and tasks must not be created or deleted while a save is in progress.
 * @return false, without touching flash, for invalid settings, an unsafe context or a busy mutex; true only after
 * the stored record reads back identical. An unchanged record is not rewritten.
 */
bool pf_settings_save(const pf_settings_t *settings);

#endif
