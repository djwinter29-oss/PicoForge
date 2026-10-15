#include "msc.h"

#include <string.h>

#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "drivers/sd_card/sd_card_block.h"
#include "drivers/sd_card/sd_card_msc.h"
#include "middleware/storage/storage.h"
#include "middleware/storage/storage_worker.h"

static bool msc_enabled;
static bool msc_ejected;
static bool removal_prevented;
static bool request_pending;
static bool requested_enabled;
static bool transition_pending;
static bool request_rejected;
/* TinyUSB 0.18.0 overwrites READ10/WRITE10 error sense after our callback.
 * Keep only that transfer's sense until REQUEST SENSE. It is dropped when any
 * later command callback runs, on unmount, and on handled BOT/bus reset.
 * USB callbacks run on the USB task (single LUN). */
static bool transfer_sense_pending;
static uint8_t transfer_sense_key, transfer_sense_asc;

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
    transfer_sense_pending = false;
}

static int32_t transfer_result(uint8_t lun, pf_sd_card_result_t result,
                               uint32_t size, bool writing)
{
    if (result == PF_SD_CARD_OK) {
        return (int32_t)size;
    }
    bool unavailable = (result == PF_SD_CARD_NOT_READY) ||
                       (result == PF_SD_CARD_NOT_CONFIGURED);
    uint8_t key = unavailable ? SCSI_SENSE_NOT_READY : SCSI_SENSE_MEDIUM_ERROR;
    /* ASC 11: unrecovered read error; ASC 0C: write error. */
    uint8_t asc = unavailable ? 0x3au : (writing ? 0x0cu : 0x11u);
    fail_transfer(lun, key, asc);
    return -1;
}

int32_t tud_msc_request_sense_cb(uint8_t lun, void *buffer, uint16_t bufsize)
{
    /* TinyUSB supplies its 18-byte fixed response before calling us, then
     * clears its sense. Preserve that response except for the pending error. */
    if ((buffer == NULL) || (bufsize < 18u)) {
        return -1;
    }
    if ((lun == 0u) && transfer_sense_pending) {
        uint8_t *response = buffer;
        response[2] = transfer_sense_key;
        response[12] = transfer_sense_asc;
        response[13] = 0u;
        transfer_sense_pending = false;
    }
    return 18;
}

static uint8_t sector_buffer[PF_SD_CARD_BLOCK_SIZE] __attribute__((aligned(4)));

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

void pf_usb_msc_request(bool enabled)
{
    requested_enabled = enabled;
    request_pending = true;
    transition_pending = enabled != msc_enabled;
    request_rejected = false;
}

static void apply_msc_request(void)
{
    pf_sd_card_result_t result;

    if (!request_pending) {
        return;
    }
    request_pending = false;
    if (requested_enabled == msc_enabled) {
        transition_pending = false;
        return;
    }

    if (pf_storage_worker_busy()) {
        transition_pending = false;
        request_rejected = true;
        return;
    }

    if (requested_enabled) {
        if (!pf_sd_card_is_initialized()) {
            transition_pending = false;
            request_rejected = true;
            return;
        }
        if (pf_storage_is_mounted()) {
            transition_pending = false;
            request_rejected = true;
            return;
        }
        result = pf_sd_card_msc_acquire();
        if (result != PF_SD_CARD_OK) {
            transition_pending = false;
            request_rejected = true;
            return;
        }
        pf_storage_forget_filesystem_format();
        msc_ejected = false;
        removal_prevented = false;
        msc_enabled = true;
    } else {
        pf_sd_card_info_t info;
        bool media_absent;

        pf_sd_card_get_info(&info);
        media_absent = info.presence_known && !info.present &&
                       pf_storage_worker_media_absent();
        if (removal_prevented ||
            (!media_absent && (!msc_ejected || pf_sd_card_msc_sync() != PF_SD_CARD_OK))) {
            transition_pending = false;
            request_rejected = true;
            return;
        }
        msc_enabled = false;
        result = pf_sd_card_msc_release();
        if (result != PF_SD_CARD_OK) {
            msc_enabled = true;
            transition_pending = false;
            request_rejected = true;
            return;
        }
    }

    transition_pending = false;
}

void pf_usb_msc_task(void)
{
    bool was_enabled = msc_enabled;
    pf_sd_card_lock();
    apply_msc_request();
    pf_sd_card_unlock();
    if (was_enabled != msc_enabled) {
        transfer_sense_pending = false;
        (void)tud_disconnect();
        sleep_ms(250u);
        (void)tud_connect();
    }
}

static bool media_ready(uint8_t lun)
{
    if ((lun == 0u) && msc_enabled && !msc_ejected && pf_sd_card_is_initialized()) {
        return true;
    }
    set_sense(lun, SCSI_SENSE_NOT_READY, 0x3au, 0x00u);
    return false;
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16],
                        uint8_t product_rev[4])
{
    (void)lun;
    transfer_sense_pending = false;
    memcpy(vendor_id, "PicoForge", 8u);
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
    *block_count = media_ready(lun) ? pf_sd_card_block_count() : 0u;
    *block_size = PF_SD_CARD_BLOCK_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start,
                           bool load_eject)
{
    (void)power_condition;
    transfer_sense_pending = false;
    if ((lun != 0u) || !msc_enabled) {
        return false;
    }
    if (load_eject) {
        if (start) {
            if (!pf_sd_card_is_initialized()) {
                set_sense(lun, SCSI_SENSE_NOT_READY, 0x3au, 0x00u);
                return false;
            }
            msc_ejected = false;
        } else {
            if (removal_prevented) {
                set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x53u, 0x02u);
                return false;
            }
            if (msc_ejected) {
                return true;
            }
            if (!media_ready(lun)) {
                return false;
            }
            pf_sd_card_result_t result = pf_sd_card_msc_sync();
            if (result != PF_SD_CARD_OK) {
                if ((result == PF_SD_CARD_NOT_READY) || (result == PF_SD_CARD_NOT_CONFIGURED)) {
                    set_sense(lun, SCSI_SENSE_NOT_READY, 0x3au, 0x00u);
                } else {
                    set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0cu, 0x00u);
                }
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
        set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x24u, 0x00u);
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
     * ponytail: TinyUSB's current 512-byte staging limits actual BOT callbacks;
     * keep synchronous calls until a pre-CSW flush/error boundary permits safe
     * larger staging or aggregation. The callback adds no one-sector ceiling. */
    bool full_sectors = (offset == 0u) && (size % PF_SD_CARD_BLOCK_SIZE == 0u);
    if ((buffer == NULL) || (size > INT32_MAX) ||
        (offset > PF_SD_CARD_BLOCK_SIZE) ||
        (!full_sectors && (size > PF_SD_CARD_BLOCK_SIZE - offset))) {
        fail_transfer(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x24u);
        return false;
    }
    uint32_t capacity = pf_sd_card_block_count();
    uint32_t blocks = full_sectors ? size / PF_SD_CARD_BLOCK_SIZE : 1u;
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
    if ((offset == 0u) && (bufsize % PF_SD_CARD_BLOCK_SIZE == 0u)) {
        return transfer_result(lun, pf_sd_card_msc_read_blocks(
                                   lba, buffer, bufsize / PF_SD_CARD_BLOCK_SIZE),
                               bufsize, false);
    }
    pf_sd_card_result_t result = pf_sd_card_msc_read_blocks(lba, sector_buffer, 1u);
    if (result != PF_SD_CARD_OK) {
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
    if ((offset == 0u) && (bufsize % PF_SD_CARD_BLOCK_SIZE == 0u)) {
        uint32_t count = bufsize / PF_SD_CARD_BLOCK_SIZE;
        pf_sd_card_result_t result = pf_sd_card_msc_write_blocks(lba, buffer, count);
        /* SDIO CMD25 can retain an open stream. Close it before reporting
         * success, while errors can still reach TinyUSB. Never retry or sync
         * a failed, potentially accepted write. */
        if ((result == PF_SD_CARD_OK) && (count > 1u)) {
            result = pf_sd_card_msc_sync();
        }
        return transfer_result(lun, result, bufsize, true);
    }
    pf_sd_card_result_t result = pf_sd_card_msc_read_blocks(lba, sector_buffer, 1u);
    if (result != PF_SD_CARD_OK) {
        return transfer_result(lun, result, bufsize, false);
    }
    memcpy(sector_buffer + offset, buffer, bufsize);
    return transfer_result(lun, pf_sd_card_msc_write_blocks(lba, sector_buffer, 1u),
                           bufsize, true);
}

/* Reserve validation, capacity, backend completion and partial-sector RMW
 * as one transaction against owner handoff/removal and shared sector scratch.
 * The facade mutex is recursive; no callback buffer survives this call. */
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                         void *buffer, uint32_t bufsize)
{
    pf_sd_card_lock();
    int32_t result = read10(lun, lba, offset, buffer, bufsize);
    pf_sd_card_unlock();
    return result;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          uint8_t *buffer, uint32_t bufsize)
{
    pf_sd_card_lock();
    int32_t result = write10(lun, lba, offset, buffer, bufsize);
    pf_sd_card_unlock();
    return result;
}

void tud_msc_write10_complete_cb(uint8_t lun)
{
    /* Both backends finish response checking and card busy before returning
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
            set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x24u, 0x00u);
            return -1;
        }
        if (!media_ready(lun)) {
            return -1;
        }
        uint32_t lba = ((uint32_t)scsi_cmd[2] << 24u) |
                       ((uint32_t)scsi_cmd[3] << 16u) |
                       ((uint32_t)scsi_cmd[4] << 8u) | scsi_cmd[5];
        uint32_t count = ((uint32_t)scsi_cmd[7] << 8u) | scsi_cmd[8];
        uint32_t capacity = pf_sd_card_block_count();
        /* A zero count covers the remainder of the medium, not an invalid LBA. */
        if ((lba >= capacity) || (count > capacity - lba)) {
            set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21u, 0x00u);
            return -1;
        }
        pf_sd_card_result_t result = pf_sd_card_msc_sync();
        if (result != PF_SD_CARD_OK) {
            if ((result == PF_SD_CARD_NOT_READY) || (result == PF_SD_CARD_NOT_CONFIGURED)) {
                set_sense(lun, SCSI_SENSE_NOT_READY, 0x3au, 0x00u);
            } else {
                set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0cu, 0x00u);
            }
            return -1;
        }
        return 0;
    }
    set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20u, 0x00u);
    return -1;
}
