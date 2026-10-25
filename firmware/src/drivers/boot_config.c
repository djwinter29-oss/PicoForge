/**
 * @file boot_config.c
 * @brief Boot configuration: one RAM record, validation, and private flash persistence.
 */
#include "drivers/boot_config.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "hardware/flash.h"
#include "pico/flash.h"
#include "task.h"

#define MAGIC 0x46434650u /* "PFCF" in little-endian byte order */
#define FORMAT_VERSION 1u
#define SAVE_TIMEOUT_MS 1000u
#define LOCKOUT_RECLAIM_TIMEOUT_MS 1000u

#define BOOT_CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

_Static_assert(BOOT_CONFIG_FLASH_OFFSET % FLASH_SECTOR_SIZE == 0u, "config sector must be aligned");

/**
 * @brief v1 flash record, stored little-endian with one byte per profile.
 *
 * ponytail: written and read as raw memory, which matches the flash byte order only on a little-endian CPU (RP2040,
 * RP2350 and the host tests). Every field is naturally aligned, so there is no padding; the static assert guards the
 * layout. Upgrade path for a big-endian target: serialize each field explicitly.
 */
typedef struct {
    uint32_t magic; /**< MAGIC. */
    uint16_t version; /**< FORMAT_VERSION. */
    uint16_t size; /**< sizeof(record_t). */
    uint8_t sysclk_profile; /**< pf_sysclk_profile_t value, independent of enum storage size. */
    uint8_t sdcard_profile; /**< Shared pf_storage_speed_t value for either transport. */
    uint16_t reserved; /**< Must be zero; keeps the CRC naturally aligned. */
    uint32_t crc; /**< IEEE CRC-32 of all preceding bytes. */
} record_t;

_Static_assert(sizeof(record_t) == 16u && offsetof(record_t, crc) == 12u, "v1 record layout");

static record_t config; /**< RAM record; zero profiles select defaults, and save refreshes metadata and CRC. */
static bool loaded; /**< The saved record has been read once. */
/** Prevents access while flash_safe_execute is active. */
static bool save_in_progress;
/** Task count before flash_safe_execute; valid while lockout_pending. */
static UBaseType_t tasks_before_lockout;
/** True while the SDK's static lockout task may not have been reclaimed yet. */
static bool lockout_pending;

static const uint8_t *stored_data(void)
{
    return (const uint8_t *)(XIP_BASE + BOOT_CONFIG_FLASH_OFFSET);
}

static bool task_context(void)
{
    return !xPortIsInsideInterrupt() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
}

/** Runs with interrupts disabled and the other core held out of flash. */
static void erase_and_program(void *param)
{
    flash_range_erase(BOOT_CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(BOOT_CONFIG_FLASH_OFFSET, param, FLASH_PAGE_SIZE);
}

/** Wait for the idle task to reclaim the SDK's static lockout TCB. */
static bool lockout_reclaimed(uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    while (uxTaskGetNumberOfTasks() != tasks_before_lockout) {
        if ((TickType_t)(xTaskGetTickCount() - start) >= timeout) {
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

/** @brief IEEE 802.3 CRC-32 (reflected, init and final XOR 0xFFFFFFFF). */
static uint32_t crc32(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t crc = 0xffffffffu;
    while (size-- > 0u) {
        crc ^= *bytes++;
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/** @brief Reject malformed records before publishing their profiles to the RAM copy. */
static bool record_valid(const record_t *record)
{
    return record->magic == MAGIC && record->version == FORMAT_VERSION && record->size == sizeof(record_t) &&
           record->crc == crc32(record, offsetof(record_t, crc)) && record->sysclk_profile <= PF_SYSCLK_250MHZ &&
           record->sdcard_profile <= PF_STORAGE_SPEED_HIGH && record->reserved == 0u;
}

/**
 * @brief Load the saved record into the RAM copy once.
 *
 * ponytail: lazy load on the first getter. The boot clock hook runs before the scheduler, so in firmware the first
 * call reads flash directly; task-context loading only serves hosts/tests that skip the hook. A
 * malformed record latches defaults until reboot; an unsafe context leaves loading pending for a safe caller.
 */
static void ensure_loaded(void)
{
    record_t record;
    if (loaded) {
        return;
    }
    if (save_in_progress || xPortIsInsideInterrupt() ||
        (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED && !task_context())) {
        return;
    }
    memcpy(&record, stored_data(), sizeof(record));
    if (record_valid(&record)) {
        config = record;
    }
    loaded = true;
}

pf_sysclk_profile_t pf_boot_config_sysclk(void)
{
    ensure_loaded();
    return (pf_sysclk_profile_t)config.sysclk_profile;
}

bool pf_boot_config_set_sysclk(pf_sysclk_profile_t profile)
{
    ensure_loaded();
    if (!loaded || (unsigned)profile > PF_SYSCLK_250MHZ) {
        return false;
    }
    config.sysclk_profile = (uint8_t)profile;
    return true;
}

pf_storage_speed_t pf_boot_config_storage_speed(void)
{
    ensure_loaded();
    return (pf_storage_speed_t)config.sdcard_profile;
}

bool pf_boot_config_set_storage_speed(pf_storage_speed_t speed)
{
    ensure_loaded();
    if (!loaded || (speed != PF_STORAGE_SPEED_DEFAULT && speed != PF_STORAGE_SPEED_HIGH)) {
        return false;
    }
    config.sdcard_profile = (uint8_t)speed;
    return true;
}

bool pf_boot_config_save(void)
{
    uint8_t page[FLASH_PAGE_SIZE];
    if (!task_context() || save_in_progress || (lockout_pending && !lockout_reclaimed(0u))) {
        return false;
    }
    lockout_pending = false;
    ensure_loaded();
    config.magic = MAGIC;
    config.version = FORMAT_VERSION;
    config.size = sizeof(config);
    config.reserved = 0u;
    config.crc = crc32(&config, offsetof(record_t, crc));
    if (memcmp(stored_data(), &config, sizeof(config)) == 0) {
        return true;
    }
    memset(page, 0xff, sizeof(page));
    memcpy(page, &config, sizeof(config));
    save_in_progress = true;
    tasks_before_lockout = uxTaskGetNumberOfTasks();
    int rc = flash_safe_execute(erase_and_program, page, SAVE_TIMEOUT_MS);
    lockout_pending = !lockout_reclaimed(LOCKOUT_RECLAIM_TIMEOUT_MS);
    save_in_progress = false;
    return rc == PICO_OK && memcmp(stored_data(), page, sizeof(config)) == 0;
}
