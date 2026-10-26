/**
 * @file
 * @brief USB mass-storage ownership state machine and SCSI callbacks for the single-LUN SD card.
 *
 * Ownership moves between firmware (FatFs) and the USB host only from the USB task, through pf_usb_msc_request() and
 * friends followed by pf_usb_msc_task(). TinyUSB callbacks also run on the USB task, so the state below needs no
 * locking; storage access inside callbacks is serialized with the storage facade lock.
 */
#include "usb_msc.h"

#include <string.h>

#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include "tusb.h"
#include "platform/storage/storage.h"
#include "platform/storage/fatfs_volume.h"
#include "app/storage/storage_worker.h"

/** SCSI logical block size; the SD backends only expose 512-byte sectors. */
#define MSC_SECTOR_SIZE 512u
/** @name SPC additional sense codes used by the callbacks @{ */
#define ASC_WRITE_ERROR 0x0cu /**< Write error (medium error key). */
#define ASC_UNRECOVERED_READ_ERROR 0x11u /**< Unrecovered read error (medium error key). */
#define ASC_INVALID_COMMAND 0x20u /**< Invalid command operation code. */
#define ASC_LBA_OUT_OF_RANGE 0x21u /**< Logical block address out of range. */
#define ASC_INVALID_CDB_FIELD 0x24u /**< Invalid field in CDB. */
#define ASC_MEDIUM_NOT_PRESENT 0x3au /**< Medium not present. */
#define ASC_REMOVAL_PREVENTED 0x53u /**< Media removal prevented; ASCQ 02 is the qualifier. */
#define ASCQ_REMOVAL_PREVENTED 0x02u /**< ASCQ paired with ASC_REMOVAL_PREVENTED. */
/** @} */
/** SPC fixed-format sense data layout. */
#define SCSI_SENSE_FIXED_SIZE 18u
#define SCSI_SENSE_KEY_OFFSET 2u
#define SCSI_SENSE_ASC_OFFSET 12u
#define SCSI_SENSE_ASCQ_OFFSET 13u
/** Detach time before reconnecting after an ownership change, so the host notices the media change. */
#define MSC_RECONNECT_DETACH_US 250000u
/** Longest wait for the host to fetch a deferred Feature 20 result before an ownership change proceeds. */
#define MSC_HID_RESULT_DEADLINE_US 500000u
/** Pause after the host fetched the result, so the GET completes before USB detaches. */
#define MSC_HID_RESULT_SETTLE_US 10000u

static bool msc_enabled; /**< USB host owns the card. */
static bool msc_ejected; /**< Host ejected the medium. */
static bool removal_prevented; /**< Host holds PREVENT MEDIUM REMOVAL (volatile, cleared on bus reset). */
static bool request_pending; /**< An ownership request awaits pf_usb_msc_task(). */
static bool requested_enabled; /**< Target owner of the pending request: true for USB. */
static bool transition_pending; /**< Reported to the host through HID: an ownership change is in progress. */
static bool request_rejected; /**< The last valid request failed; reported through HID. */
static bool recovery_requested; /**< Pending request is a faulted-media detach, not a clean eject. */
static bool unmount_pending; /**< Worker handoff preparation must finish before ownership changes. */
static bool force_return_requested; /**< Explicit unsafe command; never inferred from normal disable. */
static bool remount_pending; /**< Forced return awaits the sole worker's mount result. */
static bool reconnect_pending; /**< USB is detached after an ownership change and reconnects at the deadline. */
static uint64_t reconnect_at_us; /**< time_us_64() value at which to reconnect. */
static bool hid_result_deferred; /**< Delay the change until the host fetched the HID result or the deadline passes. */
static bool hid_result_read; /**< The host fetched the deferred result. */
static uint8_t hid_result_transaction_id; /**< Transaction whose result is awaited. */
static uint64_t hid_result_deadline_us; /**< Give-up time for the deferral. */
static uint64_t hid_result_settle_us; /**< Earliest change time after the result was read. */
/* TinyUSB 0.18.0 overwrites READ10/WRITE10 error sense after our callback.
 * Keep only that transfer's sense until REQUEST SENSE. It is dropped when any
 * later command callback runs, on unmount, and on handled BOT/bus reset.
 * USB callbacks run on the USB task (single LUN). */
static bool transfer_sense_pending; /**< A transfer error sense awaits REQUEST SENSE. */
static uint8_t transfer_sense_key; /**< Sense key to report. */
static uint8_t transfer_sense_asc; /**< Additional sense code to report. */

static void set_sense(uint8_t lun, uint8_t key, uint8_t asc, uint8_t ascq)
{
    transfer_sense_pending = false;
    tud_msc_set_sense(lun, key, asc, ascq);
}

static void fail_transfer(uint8_t lun, uint8_t key, uint8_t asc)
{
    set_sense(lun, key, asc, 0u);
    transfer_sense_key = key;
    transfer_sense_asc = asc;
    transfer_sense_pending = true;
}

/* GNU ld wraps the built-in MSC driver's external references in usbd.c.
 * Both boundaries execute in tud_task, after queued events ahead of reset.
 * Enqueue-time hooks can run in ISR context and are not reset barriers. */
extern void __real_mscd_reset(uint8_t rhport);
extern bool __real_mscd_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                      tusb_control_request_t const *request);

void __wrap_mscd_reset(uint8_t rhport)
{
    __real_mscd_reset(rhport);
    /* Volatile SCSI prevention ends on bus reset/disconnect, not BOT transport reset. */
    removal_prevented = false;
    transfer_sense_pending = false;
}

bool __wrap_mscd_control_xfer_cb(uint8_t rhport, uint8_t stage,
                               tusb_control_request_t const *request)
{
    bool accepted = __real_mscd_control_xfer_cb(rhport, stage, request);
    /* Match the pinned driver's accepted BOT reset at SETUP only. Routing
     * and validation remain TinyUSB's responsibility; ACK is too late. */
    if (accepted && stage == CONTROL_STAGE_SETUP &&
        request->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS &&
        request->bRequest == MSC_REQ_RESET &&
        request->wValue == 0u && request->wLength == 0u) {
        transfer_sense_pending = false;
    }
    return accepted;
}

void tud_mount_cb(void)
{
    transfer_sense_pending = false;
}

void tud_umount_cb(void)
{
    removal_prevented = false;
    transfer_sense_pending = false;
}

static int32_t transfer_result(uint8_t lun, pf_block_result_t result,
                               uint32_t size, bool writing)
{
    if (result == PF_BLOCK_OK) {
        return (int32_t)size;
    }
    bool unavailable = result == PF_BLOCK_NOT_READY;
    uint8_t key = unavailable ? SCSI_SENSE_NOT_READY : SCSI_SENSE_MEDIUM_ERROR;
    uint8_t asc = unavailable ? ASC_MEDIUM_NOT_PRESENT
                                  : (writing ? ASC_WRITE_ERROR : ASC_UNRECOVERED_READ_ERROR);
    fail_transfer(lun, key, asc);
    return -1;
}

int32_t tud_msc_request_sense_cb(uint8_t lun, void *buffer, uint16_t bufsize)
{
    /* TinyUSB supplies its 18-byte fixed response before calling us, then
     * clears its sense. Preserve that response except for the pending error. */
    if ((buffer == NULL) || (bufsize < SCSI_SENSE_FIXED_SIZE)) {
        return -1;
    }
    if ((lun == 0u) && transfer_sense_pending) {
        uint8_t *response = buffer;
        response[SCSI_SENSE_KEY_OFFSET] = transfer_sense_key;
        response[SCSI_SENSE_ASC_OFFSET] = transfer_sense_asc;
        response[SCSI_SENSE_ASCQ_OFFSET] = 0u;
        transfer_sense_pending = false;
    }
    return SCSI_SENSE_FIXED_SIZE;
}

static uint8_t sector_buffer[MSC_SECTOR_SIZE] __attribute__((aligned(4)));

bool pf_usb_msc_is_enabled(void)
{
    return msc_enabled;
}

bool pf_usb_msc_is_ejected(void)
{
    return msc_ejected;
}

bool pf_usb_msc_transition_pending(void)
{
    return transition_pending;
}

bool pf_usb_msc_request_rejected(void)
{
    return request_rejected;
}

bool pf_usb_msc_request(bool enabled)
{
    if (remount_pending) {
        request_rejected = true;
        return false;
    }
    force_return_requested = false;
    recovery_requested = false;
    hid_result_deferred = false;
    hid_result_read = false;
    requested_enabled = enabled;
    request_pending = true;
    transition_pending = enabled != msc_enabled;
    request_rejected = false;
    return true;
}

void pf_usb_msc_request_recovery(void)
{
    if (remount_pending) {
        return;
    }
    hid_result_deferred = false;
    hid_result_read = false;
    force_return_requested = false;
    recovery_requested = true;
    request_pending = true;
    transition_pending = true;
    request_rejected = false;
}

bool pf_usb_msc_request_force_return(void)
{
    const bool accepted = pf_usb_msc_request(false);
    if (accepted) force_return_requested = true;
    return accepted;
}

void pf_usb_msc_defer_for_hid_result(uint8_t transaction_id)
{
    if (!request_pending || !transition_pending || transaction_id == 0u) {
        return;
    }
    hid_result_deferred = true;
    hid_result_read = false;
    hid_result_transaction_id = transaction_id;
    hid_result_deadline_us = time_us_64() + MSC_HID_RESULT_DEADLINE_US;
}

void pf_usb_msc_note_hid_result_read(uint8_t transaction_id)
{
    if (hid_result_deferred && transaction_id == hid_result_transaction_id) {
        hid_result_read = true;
        hid_result_settle_us = time_us_64() + MSC_HID_RESULT_SETTLE_US;
    }
}

/** @brief Reconnect USB after the delay; a failed tud_connect() is retried by pf_usb_msc_task(). */
static void schedule_reconnect(uint64_t delay_us)
{
    reconnect_at_us = time_us_64() + delay_us;
    reconnect_pending = true;
}

/** @brief Abort the pending ownership change and report it as rejected. */
static void reject_request(void)
{
    transition_pending = false;
    request_rejected = true;
}

/** @brief Detach and hand the card to the worker for faulted-media recovery; never reported as a clean eject. */
static void apply_recovery(void)
{
    recovery_requested = false;
    if (!msc_enabled || pf_storage_msc_ready() || removal_prevented ||
        pf_storage_worker_busy() || pf_fatfs_volume_is_mounted() ||
        !pf_storage_worker_recover_media()) {
        reject_request();
        return;
    }
    /* The queued worker waits for our reservation. Detach before handing
     * it the card; this is never reported as successful SCSI eject/sync. */
    (void)tud_disconnect();
    if (pf_storage_msc_release_for_recovery() != PF_BLOCK_OK) {
        reject_request();
        schedule_reconnect(0u);
        return;
    }
    msc_enabled = false;
    transition_pending = false;
}

/** @brief Explicit unsafe return: detach, then let the worker remount once. */
static void apply_force_return(void)
{
    force_return_requested = false;
    /* Device sync cannot flush writes still cached by the host. A device-side
     * failure keeps USB ownership; only the explicit command bypasses host eject. */
    if (pf_fatfs_volume_is_mounted() || pf_storage_msc_sync() != PF_BLOCK_OK || !tud_disconnect()) {
        reject_request();
        return;
    }
    if (!pf_storage_worker_return_from_usb() || pf_storage_msc_release() != PF_BLOCK_OK) {
        reject_request();
        schedule_reconnect(0u);
        return;
    }
    msc_enabled = false;
    remount_pending = true;
}

/** @brief Give the card to the USB host, first asking the worker to unmount FatFs if needed. */
static void apply_enable(void)
{
    if (!pf_storage_msc_ready()) {
        reject_request();
        return;
    }
    if (pf_fatfs_volume_is_mounted()) {
        if (!pf_storage_worker_unmount_for_usb()) {
            reject_request();
            return;
        }
        unmount_pending = true;
        request_pending = true;
        return;
    }
    if (pf_storage_msc_acquire() != PF_BLOCK_OK) {
        reject_request();
        return;
    }
    pf_fatfs_volume_forget_filesystem_format();
    msc_ejected = false;
    removal_prevented = false;
    msc_enabled = true;
    transition_pending = false;
}

/** @brief Return the card to firmware once the host ejected it or the media is gone. */
static void apply_disable(void)
{
    bool media_absent = pf_storage_worker_media_absent();

    if (removal_prevented ||
        (!media_absent && (!msc_ejected || pf_storage_msc_sync() != PF_BLOCK_OK))) {
        reject_request();
        return;
    }
    msc_enabled = false;
    if (pf_storage_msc_release() != PF_BLOCK_OK) {
        msc_enabled = true;
        reject_request();
        return;
    }
    transition_pending = false;
}

/** @brief Execute the pending ownership request; called with the storage lock held. */
static void apply_msc_request(void)
{
    if (!request_pending) {
        return;
    }
    if (unmount_pending) {
        if (pf_storage_worker_busy()) {
            return;
        }
        unmount_pending = false;
        if (requested_enabled && !recovery_requested && pf_fatfs_volume_is_mounted()) {
            request_pending = false;
            reject_request();
            return;
        }
    }
    request_pending = false;
    if (recovery_requested) {
        apply_recovery();
    } else if (requested_enabled == msc_enabled) {
        transition_pending = false;
    } else if (pf_storage_worker_busy()) {
        reject_request();
    } else if (force_return_requested) {
        apply_force_return();
    } else if (requested_enabled) {
        apply_enable();
    } else {
        apply_disable();
    }
}

void pf_usb_msc_task(void)
{
    /* Runs first so a pending reconnect is never starved by the deferral or remount waits below. Cleared only on
     * success: a failed tud_connect() retries next cycle instead of leaving the host without a device. */
    if (reconnect_pending && time_us_64() >= reconnect_at_us) {
        reconnect_pending = !tud_connect();
    }
    if (hid_result_deferred) {
        uint64_t now_us = time_us_64();
        if ((!hid_result_read && now_us < hid_result_deadline_us) ||
            (hid_result_read && now_us < hid_result_settle_us)) {
            return;
        }
        hid_result_deferred = false;
    }
    if (remount_pending) {
        if (!pf_storage_worker_busy()) {
            remount_pending = false;
            transition_pending = false;
            request_rejected = !pf_fatfs_volume_is_mounted();
        }
        return;
    }
    if (!request_pending) {
        return;
    }
    if (!pf_storage_try_lock()) {
        return;
    }
    bool was_enabled = msc_enabled;
    bool detached_first = recovery_requested || force_return_requested;
    apply_msc_request();
    pf_storage_unlock();
    if (was_enabled != msc_enabled) {
        transfer_sense_pending = false;
        if (!detached_first) {
            (void)tud_disconnect();
        }
        /* Reconnect later instead of sleeping, so HID, CDC and storage replies keep being serviced. */
        schedule_reconnect(MSC_RECONNECT_DETACH_US);
    }
}

/** @brief Report "medium not present" for the LUN and return false. */
static bool set_not_ready(uint8_t lun)
{
    set_sense(lun, SCSI_SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0x00u);
    return false;
}

static bool media_ready(uint8_t lun)
{
    if ((lun == 0u) && msc_enabled && !msc_ejected && pf_storage_msc_ready()) {
        return true;
    }
    return set_not_ready(lun);
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16],
                        uint8_t product_rev[4])
{
    (void)lun;
    transfer_sense_pending = false;
    /* SPC INQUIRY fields are fixed width and space padded; the vendor name is deliberately truncated to 8. */
    memset(vendor_id, ' ', 8u);
    memset(product_id, ' ', 16u);
    memset(product_rev, ' ', 4u);
    memcpy(vendor_id, "PicoForg", 8u);
    memcpy(product_id, "SD Card", 7u);
    memcpy(product_rev, "1.0", 3u);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    transfer_sense_pending = false;
    return media_ready(lun);
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    transfer_sense_pending = false;
    *block_count = media_ready(lun) ? pf_storage_msc_count() : 0u;
    *block_size = MSC_SECTOR_SIZE;
}

/* Eject and SYNCHRONIZE CACHE share the same pre-CSW sync contract. */
static bool sync_media(uint8_t lun)
{
    pf_block_result_t result = pf_storage_msc_sync();
    if (result == PF_BLOCK_OK) {
        return true;
    }
    bool unavailable = result == PF_BLOCK_NOT_READY;
    set_sense(lun, unavailable ? SCSI_SENSE_NOT_READY : SCSI_SENSE_MEDIUM_ERROR,
              unavailable ? ASC_MEDIUM_NOT_PRESENT : ASC_WRITE_ERROR, 0x00u);
    return false;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start,
                           bool load_eject)
{
    (void)power_condition;
    transfer_sense_pending = false;
    if ((lun != 0u) || !msc_enabled) {
        return set_not_ready(lun);
    }
    if (load_eject) {
        if (start) {
            if (!pf_storage_msc_ready()) {
                return set_not_ready(lun);
            }
            msc_ejected = false;
        } else {
            if (removal_prevented) {
                set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, ASC_REMOVAL_PREVENTED, ASCQ_REMOVAL_PREVENTED);
                return false;
            }
            if (msc_ejected) {
                return true;
            }
            if (!media_ready(lun)) {
                return false;
            }
            if (!sync_media(lun)) {
                return false;
            }
            msc_ejected = true;
        }
    }
    return true;
}

bool tud_msc_prevent_allow_medium_removal_cb(uint8_t lun, uint8_t prohibit_removal,
                                             uint8_t control)
{
    transfer_sense_pending = false;
    if (lun != 0u) {
        return false;
    }
    /* Persistent prevention (2/3) is unsupported; reserved bits are invalid. */
    if ((prohibit_removal > 1u) || (control != 0u)) {
        set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, ASC_INVALID_CDB_FIELD, 0x00u);
        return false;
    }
    removal_prevented = prohibit_removal != 0u;
    return true;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    transfer_sense_pending = false;
    return (lun == 0u) && msc_enabled && !msc_ejected;
}

static bool transfer_is_valid(uint8_t lun, uint32_t lba, uint32_t offset,
                              const void *buffer, uint32_t size)
{
    transfer_sense_pending = false;
    if (!media_ready(lun)) {
        return false;
    }
    /* Full sectors may be batched; partial transfers must fit one sector.
     * ponytail: TinyUSB's current MSC_SECTOR_SIZE staging limits actual BOT callbacks;
     * keep synchronous calls until a pre-CSW flush/error boundary permits safe
     * larger staging or aggregation. The callback adds no one-sector ceiling. */
    bool full_sectors = (offset == 0u) && (size % MSC_SECTOR_SIZE == 0u);
    if ((buffer == NULL) || (size > INT32_MAX) ||
        (offset > MSC_SECTOR_SIZE) ||
        (!full_sectors && (size > MSC_SECTOR_SIZE - offset))) {
        fail_transfer(lun, SCSI_SENSE_ILLEGAL_REQUEST, ASC_INVALID_CDB_FIELD);
        return false;
    }
    uint32_t capacity = pf_storage_msc_count();
    uint32_t blocks = full_sectors ? size / MSC_SECTOR_SIZE : 1u;
    if ((lba >= capacity) || (blocks > capacity - lba)) {
        fail_transfer(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21u);
        return false;
    }
    return true;
}

static int32_t read10(uint8_t lun, uint32_t lba, uint32_t offset,
                      void *buffer, uint32_t bufsize)
{
    if (!transfer_is_valid(lun, lba, offset, buffer, bufsize)) {
        return -1;
    }
    if (bufsize == 0u) {
        return 0;
    }
    if ((offset == 0u) && (bufsize % MSC_SECTOR_SIZE == 0u)) {
        return transfer_result(lun, pf_storage_msc_read(
                                   lba, buffer, bufsize / MSC_SECTOR_SIZE),
                               bufsize, false);
    }
    pf_block_result_t result = pf_storage_msc_read(lba, sector_buffer, 1u);
    if (result != PF_BLOCK_OK) {
        return transfer_result(lun, result, bufsize, false);
    }
    memcpy(buffer, sector_buffer + offset, bufsize);
    return (int32_t)bufsize;
}

static int32_t write10(uint8_t lun, uint32_t lba, uint32_t offset,
                       uint8_t *buffer, uint32_t bufsize)
{
    if (!transfer_is_valid(lun, lba, offset, buffer, bufsize)) {
        return -1;
    }
    if (bufsize == 0u) {
        return 0;
    }
    if ((offset == 0u) && (bufsize % MSC_SECTOR_SIZE == 0u)) {
        uint32_t count = bufsize / MSC_SECTOR_SIZE;
        pf_block_result_t result = pf_storage_msc_write(lba, buffer, count);
        /* SDIO CMD25 can retain an open stream. Sync batched writes before reporting
         * success, while errors can still reach TinyUSB. Never retry or sync
         * a failed, potentially accepted write. */
        if ((result == PF_BLOCK_OK) && (count > 1u)) {
            result = pf_storage_msc_sync();
        }
        return transfer_result(lun, result, bufsize, true);
    }
    pf_block_result_t result = pf_storage_msc_read(lba, sector_buffer, 1u);
    if (result != PF_BLOCK_OK) {
        return transfer_result(lun, result, bufsize, false);
    }
    memcpy(sector_buffer + offset, buffer, bufsize);
    return transfer_result(lun, pf_storage_msc_write(lba, sector_buffer, 1u),
                           bufsize, true);
}

/* Reserve validation, capacity, backend completion and partial-sector RMW
 * as one transaction against owner handoff/removal and shared sector scratch.
 * The facade mutex is recursive; no callback buffer survives this call. */
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                         void *buffer, uint32_t bufsize)
{
    pf_storage_lock();
    int32_t result = read10(lun, lba, offset, buffer, bufsize);
    pf_storage_unlock();
    return result;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          uint8_t *buffer, uint32_t bufsize)
{
    pf_storage_lock();
    int32_t result = write10(lun, lba, offset, buffer, bufsize);
    pf_storage_unlock();
    return result;
}

void tud_msc_write10_complete_cb(uint8_t lun)
{
    /* Selected storage completes writes synchronously before returning
     * WRITE10 success. TinyUSB calls this after CSW, too late to report errors.
     * Explicit SYNCHRONIZE CACHE and eject remain checked sync boundaries. */
    (void)lun;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer,
                        uint16_t bufsize)
{
    (void)buffer;
    transfer_sense_pending = false;
    /* SYNCHRONIZE CACHE(10): synchronous only; reject IMMED and reserved
     * bits rather than promising an asynchronous operation we cannot provide.
     * LBA/count are safely covered by syncing the entire device. */
    if (scsi_cmd[0] == 0x35u) {
        if ((scsi_cmd[1] != 0u) || (scsi_cmd[6] != 0u) ||
            (scsi_cmd[9] != 0u) || (bufsize != 0u)) {
            set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, ASC_INVALID_CDB_FIELD, 0x00u);
            return -1;
        }
        if (!media_ready(lun)) {
            return -1;
        }
        uint32_t lba = ((uint32_t)scsi_cmd[2] << 24u) |
                       ((uint32_t)scsi_cmd[3] << 16u) |
                       ((uint32_t)scsi_cmd[4] << 8u) | scsi_cmd[5];
        uint32_t count = ((uint32_t)scsi_cmd[7] << 8u) | scsi_cmd[8];
        uint32_t capacity = pf_storage_msc_count();
        /* A zero count covers the remainder of the medium, not an invalid LBA. */
        if ((lba >= capacity) || (count > capacity - lba)) {
            set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0x00u);
            return -1;
        }
        if (!sync_media(lun)) {
            return -1;
        }
        return 0;
    }
    set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, ASC_INVALID_COMMAND, 0x00u);
    return -1;
}
