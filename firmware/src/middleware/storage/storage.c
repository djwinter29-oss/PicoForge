/** @file
 * @brief Storage service over the SD driver: lifecycle, presence/automount policy, owner gates and status.
 */
#include "middleware/storage/storage.h"

#include <stddef.h>
#include <string.h>

#include "drivers/sdcard/sd.h"
#include "drivers/sdcard/sd_board.h"
#include "drivers/sdcard/sd_port.h"
#include "middleware/storage/fatfs_volume.h"
#include "middleware/logging/logging.h"
#include "drivers/system.h"

static pf_sd_config_t media_config; /**< Board transport borrowed for application lifetime. */
static bool configured; /**< Execution-port and board configuration are available. */
static bool usb_owned; /**< USB MSC exclusively owns the medium; firmware I/O is rejected. */
static bool reinit_after_usb_owner; /**< Reinitialize before mounting after a USB release. */
static pf_storage_operation_t last_failure; /**< Operation of the last failure; successful I/O never clears it. */
static pf_block_result_t last_failure_result; /**< Result of the last failure. */

/** @brief Map SD results to block results without relying on enum numeric equivalence. */
static pf_block_result_t block_result(pf_sd_result_t result)
{
    switch (result) {
    case PF_SD_OK: return PF_BLOCK_OK;
    case PF_SD_INVALID_ARGUMENT: return PF_BLOCK_INVALID_ARGUMENT;
    case PF_SD_NOT_CONFIGURED:
    case PF_SD_NOT_READY: return PF_BLOCK_NOT_READY;
    default: return PF_BLOCK_IO_ERROR;
    }
}

/** @brief Retain the last failure; caller holds the reservation. */
static pf_sd_result_t record_failure(pf_storage_operation_t operation, pf_sd_result_t result)
{
    if (result != PF_SD_OK) {
        last_failure = operation;
        last_failure_result = block_result(result);
    }
    return result;
}

pf_block_result_t pf_storage_setup(void)
{
    if (configured) {
        return PF_BLOCK_OK;
    }
    if (!pf_sd_board_config(&media_config)) {
        PF_LOG_WARNING("storage", "SD board configuration unavailable");
        return PF_BLOCK_NOT_READY;
    }
    if (!pf_sd_port_init(&media_config)) {
        PF_LOG_ERROR("storage", "SD execution-port initialization failed");
        return PF_BLOCK_IO_ERROR;
    }
    configured = true;
    return PF_BLOCK_OK;
}

/** @brief Initialize firmware media, keeping the SD result for logging. */
static pf_sd_result_t initialize_media(void)
{
    pf_sd_lock();
    pf_sd_result_t result;
    if (!configured) {
        result = PF_SD_NOT_CONFIGURED;
    } else if (usb_owned || pf_fatfs_volume_is_mounted()) {
        result = PF_SD_NOT_READY;
    } else {
        result = pf_sd_init(&media_config);
    }
    result = record_failure(PF_STORAGE_OPERATION_INIT, result);
    pf_sd_unlock();
    return result;
}

pf_block_result_t pf_storage_initialize(void)
{
    return block_result(initialize_media());
}

void pf_storage_get_status(pf_storage_status_t *status)
{
    if (status == NULL) {
        return;
    }
    *status = (pf_storage_status_t){
        .medium = PF_STORAGE_MEDIUM_SDCARD,
        .state = PF_STORAGE_STATE_NOT_CONFIGURED,
    };
    status->bus = pf_sd_board_pins(status->bus_gpios, &status->detect_gpio) == PF_SD_LINK_SPI ?
                  PF_STORAGE_BUS_SPI : PF_STORAGE_BUS_SDIO;
    if (!configured) {
        return;
    }
    if (!pf_sd_try_lock()) {
        status->state = PF_STORAGE_STATE_BUSY;
        return;
    }
    pf_sd_info_t info;
    pf_sd_get_info(&info);
    status->presence_known = info.presence_known;
    status->high_speed = info.high_speed;
    status->id_valid = info.cid_valid;
    memcpy(status->id, info.cid, sizeof(status->id));
    status->bus_clock_hz = info.bus_clock_hz;
    status->sector_count = info.block_count;
    status->last_failure = last_failure;
    status->last_failure_result = last_failure_result;
    status->state = info.presence_known && !info.present ? PF_STORAGE_STATE_ABSENT
        : usb_owned ? PF_STORAGE_STATE_USB
        : !info.initialized ? PF_STORAGE_STATE_UNINITIALIZED
        : pf_fatfs_volume_is_mounted() ? PF_STORAGE_STATE_MOUNTED : PF_STORAGE_STATE_READY;
    pf_sd_unlock();
}

/** @brief Keep the owner identity stable throughout one synchronous read batch. */
static pf_sd_result_t read_blocks_for_owner(bool usb, uint32_t first_block, void *buffer, uint32_t block_count)
{
    pf_sd_lock();
    pf_sd_result_t result = usb_owned != usb ? PF_SD_NOT_READY : pf_sd_read_blocks(first_block, buffer, block_count);
    result = record_failure(PF_STORAGE_OPERATION_READ, result);
    pf_sd_unlock();
    return result;
}

/** @brief Keep the owner identity stable throughout one synchronous write batch; never retry. */
static pf_sd_result_t write_blocks_for_owner(bool usb, uint32_t first_block, const void *buffer, uint32_t block_count)
{
    pf_sd_lock();
    pf_sd_result_t result = usb_owned != usb ? PF_SD_NOT_READY : pf_sd_write_blocks(first_block, buffer, block_count);
    result = record_failure(PF_STORAGE_OPERATION_WRITE, result);
    pf_sd_unlock();
    return result;
}

/** @brief Sync only for the current owner, never converting a failed flush into success. */
static pf_sd_result_t sync_for_owner(bool usb)
{
    pf_sd_lock();
    pf_sd_result_t result = usb_owned != usb ? PF_SD_NOT_READY : pf_sd_sync();
    result = record_failure(PF_STORAGE_OPERATION_SYNC, result);
    pf_sd_unlock();
    return result;
}

void pf_storage_lock(void) { pf_sd_lock(); }
bool pf_storage_try_lock(void) { return pf_sd_try_lock(); }
void pf_storage_unlock(void) { pf_sd_unlock(); }
bool pf_storage_msc_ready(void) { return pf_sd_is_initialized(); }
uint32_t pf_storage_msc_count(void) { return pf_sd_block_count(); }

pf_block_result_t pf_storage_msc_acquire(void)
{
    pf_sd_lock();
    pf_sd_result_t result = PF_SD_OK;
    if (!usb_owned) {
        if (!pf_sd_is_initialized() || pf_fatfs_volume_is_mounted()) {
            result = record_failure(PF_STORAGE_OPERATION_MSC_ACQUIRE, PF_SD_NOT_READY);
        } else {
            usb_owned = true;
        }
    }
    pf_sd_unlock();
    return block_result(result);
}

pf_block_result_t pf_storage_msc_release(void)
{
    pf_sd_lock();
    if (usb_owned) {
        reinit_after_usb_owner = true;
        usb_owned = false;
    }
    pf_sd_unlock();
    return PF_BLOCK_OK;
}

pf_block_result_t pf_storage_msc_release_for_recovery(void)
{
    pf_sd_lock();
    pf_block_result_t result = usb_owned && !pf_sd_is_initialized() ? pf_storage_msc_release() : PF_BLOCK_NOT_READY;
    pf_sd_unlock();
    return result;
}

pf_block_result_t pf_storage_msc_read(uint32_t first, void *buffer, uint32_t count)
{
    return block_result(read_blocks_for_owner(true, first, buffer, count));
}

pf_block_result_t pf_storage_msc_write(uint32_t first, const void *buffer, uint32_t count)
{
    return block_result(write_blocks_for_owner(true, first, buffer, count));
}

pf_block_result_t pf_storage_msc_sync(void)
{
    return block_result(sync_for_owner(true));
}

/** @brief Snapshot the card geometry under the driver lock; metadata never authorizes I/O. */
static pf_block_result_t media_geometry(void *context, pf_block_geometry_t *value)
{
    (void)context;
    pf_block_result_t result = PF_BLOCK_NOT_READY;
    pf_sd_lock();
    if (pf_sd_is_initialized()) {
        *value = (pf_block_geometry_t){
            .sector_count = pf_sd_block_count(),
            .sector_size = PF_SD_BLOCK_SIZE,
            .erase_sector_count = 1u,
        };
        result = PF_BLOCK_OK;
    }
    pf_sd_unlock();
    return result;
}

static pf_block_result_t media_read(void *context, uint32_t first, void *buffer, uint32_t count)
{
    (void)context;
    return block_result(read_blocks_for_owner(false, first, buffer, count));
}

static pf_block_result_t media_write(void *context, uint32_t first, const void *buffer, uint32_t count)
{
    (void)context;
    return block_result(write_blocks_for_owner(false, first, buffer, count));
}

static pf_block_result_t media_sync(void *context)
{
    (void)context;
    return block_result(sync_for_owner(false));
}

const pf_block_device_t *pf_storage_firmware_device(void)
{
    static const pf_block_device_ops_t ops = {
        .geometry = media_geometry, .read = media_read, .write = media_write, .sync = media_sync,
    };
    static const pf_block_device_t device = {.ops = &ops};
    return &device;
}

#define CARD_PRESENCE_DEBOUNCE_SAMPLES 3u
#define MEDIA_MOUNT_RETRY_MS 5000u

/* Mount policy state. Mutated only by the sole FatFs executor while holding the SD reservation,
 * so USB-task readers holding the same reservation observe a coherent snapshot. */
static bool presence_sample_valid; /**< presence_sample holds a raw card-detect sample. */
static bool presence_sample; /**< Last raw card-detect level being debounced. */
static uint8_t presence_sample_count; /**< Consecutive identical samples, saturating at the debounce count. */
static bool stable_presence_valid; /**< stable_card_present holds a debounced value. */
static bool stable_card_present; /**< Debounced card presence. */
static bool auto_mount_pending = true; /**< A mount attempt is wanted. */
static bool auto_mount_blocked; /**< Automount suspended until an explicit successful mount. */
static bool mount_retry_waiting; /**< Last attempt failed; pace the next one. */
static uint32_t last_mount_attempt_ms; /**< Uptime of the last mount attempt. */
static bool mount_failure_reported; /**< Suppress identical periodic mount failures until success. */
static unsigned last_media_result; /**< Last reported SD initialization outcome. */
static pf_fatfs_volume_result_t last_volume_result; /**< Last reported mount outcome. */
static FRESULT last_filesystem_result; /**< Last reported FatFs mount reason. */

/** @brief Attempt one mount and retain/report changed failures without flooding paced retries. */
static void attempt_auto_mount(void)
{
    unsigned media_result = 0u;
    pf_fatfs_volume_result_t volume_result = PF_FATFS_VOLUME_OK;
    FRESULT filesystem_result = FR_OK;
    if (pf_fatfs_volume_is_mounted()) {
        auto_mount_pending = false;
    } else {
        media_result = (unsigned)initialize_media();
        if (media_result != PF_SD_OK) {
            auto_mount_pending = true;
        } else {
            volume_result = pf_fatfs_volume_mount(pf_storage_firmware_device(), &filesystem_result);
            auto_mount_pending = (volume_result != PF_FATFS_VOLUME_OK) &&
                                 (volume_result != PF_FATFS_VOLUME_ALREADY_MOUNTED);
        }
    }
    if (auto_mount_pending) {
        if (!mount_failure_reported || media_result != last_media_result ||
            volume_result != last_volume_result || filesystem_result != last_filesystem_result) {
            PF_LOG_WARNING("storage", "Mount failed: media=%u volume=%u FatFs=%u",
                           (unsigned)media_result, (unsigned)volume_result,
                           (unsigned)filesystem_result);
        }
        mount_failure_reported = true;
        last_media_result = media_result;
        last_volume_result = volume_result;
        last_filesystem_result = filesystem_result;
    } else {
        mount_failure_reported = false;
    }
    last_mount_attempt_ms = pf_system_uptime_ms();
    mount_retry_waiting = auto_mount_pending;
}

/** @brief Debounce presence and run paced automount; caller holds the SD reservation. */
static void poll_locked(void)
{
    pf_sd_info_t info;
    pf_sd_get_info(&info);
    bool firmware_owned = !usb_owned;
    bool mounted_card_uninitialized = pf_fatfs_volume_is_mounted() && !info.initialized;
    if (firmware_owned && mounted_card_uninitialized && (!info.presence_known || info.present)) {
        if (pf_fatfs_volume_unmount(NULL) != PF_FATFS_VOLUME_OK) {
            return;
        }
        pf_fatfs_volume_forget_filesystem_format();
        auto_mount_pending = true;
        mount_retry_waiting = false;
    }
    if (info.presence_known) {
        if (!presence_sample_valid || (info.present != presence_sample)) {
            presence_sample = info.present;
            presence_sample_count = 1u;
            presence_sample_valid = true;
        } else if (presence_sample_count < CARD_PRESENCE_DEBOUNCE_SAMPLES) {
            ++presence_sample_count;
        }
        if (presence_sample_count < CARD_PRESENCE_DEBOUNCE_SAMPLES) {
            return;
        }
        if (!stable_presence_valid || (stable_card_present != presence_sample)) {
            stable_presence_valid = true;
            stable_card_present = presence_sample;
            auto_mount_pending = true;
            mount_retry_waiting = false;
            if (!stable_card_present) {
                pf_sd_confirm_removed();
                if (pf_fatfs_volume_is_mounted()) {
                    (void)pf_fatfs_volume_unmount(NULL);
                }
                pf_fatfs_volume_forget_filesystem_format();
                return;
            }
        }
        if (!stable_card_present) {
            return;
        }
    }
    if (!firmware_owned || auto_mount_blocked) {
        return;
    }
    if (reinit_after_usb_owner) {
        reinit_after_usb_owner = false;
        auto_mount_pending = true;
        mount_retry_waiting = false;
    }
    if (pf_fatfs_volume_is_mounted()) {
        auto_mount_pending = false;
        return;
    }
    if (!auto_mount_pending ||
        (mount_retry_waiting &&
         ((uint32_t)(pf_system_uptime_ms() - last_mount_attempt_ms) < MEDIA_MOUNT_RETRY_MS))) {
        return;
    }
    attempt_auto_mount();
}

void pf_storage_poll(void)
{
    if (!configured) {
        return;
    }
    pf_sd_lock();
    poll_locked();
    pf_sd_unlock();
}

bool pf_storage_recover(void)
{
    pf_sd_lock();
    auto_mount_blocked = true;
    auto_mount_pending = false;
    reinit_after_usb_owner = false;
    pf_sd_result_t result = initialize_media();
    pf_sd_unlock();
    if (result == PF_SD_OK) {
        PF_LOG_INFO("storage", "Media recovery completed; volume remains unmounted pending explicit fs mount");
    } else {
        PF_LOG_ERROR("storage", "Media recovery failed: result=%u; volume remains unmounted", (unsigned)result);
    }
    return result == PF_SD_OK;
}

bool pf_storage_return_from_usb(void)
{
    pf_sd_lock();
    reinit_after_usb_owner = false;
    attempt_auto_mount();
    auto_mount_blocked = auto_mount_pending;
    auto_mount_pending = false;
    if (auto_mount_blocked) {
        PF_LOG_ERROR("storage", "Forced USB return failed; volume remains unmounted; inspect before fs mount");
    } else {
        PF_LOG_WARNING("storage", "Forced USB return mounted; host cache was not flushed; filesystem may be damaged");
    }
    bool mounted = !auto_mount_blocked;
    pf_sd_unlock();
    return mounted;
}

bool pf_storage_unmount_for_usb(void)
{
    FRESULT filesystem_result = FR_OK;
    pf_sd_lock();
    pf_fatfs_volume_result_t result = pf_fatfs_volume_unmount(&filesystem_result);
    bool unmounted = result == PF_FATFS_VOLUME_OK || result == PF_FATFS_VOLUME_NOT_MOUNTED;
    if (unmounted) {
        auto_mount_pending = false;
    }
    pf_sd_unlock();
    if (!unmounted) {
        PF_LOG_ERROR("storage", "USB handoff unmount failed: volume=%u FatFs=%u",
                     (unsigned)result, (unsigned)filesystem_result);
    }
    return unmounted;
}

void pf_storage_note_command(bool was_mounted)
{
    pf_sd_lock();
    bool mounted = pf_fatfs_volume_is_mounted();
    if (was_mounted && !mounted) {
        auto_mount_pending = false;
    }
    if (mounted) {
        auto_mount_blocked = false;
        mount_failure_reported = false;
    }
    pf_sd_unlock();
}

bool pf_storage_media_absent(void)
{
    pf_sd_info_t info;
    pf_sd_lock();
    pf_sd_get_info(&info);
    bool absent = info.presence_known && !info.present && stable_presence_valid && !stable_card_present;
    pf_sd_unlock();
    return absent;
}
