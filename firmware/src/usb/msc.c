#include "msc.h"

#include <string.h>

#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "drivers/sd_card/sd_card_block.h"
#include "middleware/storage/storage.h"
#include "middleware/storage/storage_worker.h"

static bool msc_enabled;
static bool msc_ejected;
static bool removal_prevented;
static bool request_pending;
static bool requested_enabled;
static bool transition_pending;
static bool request_rejected;
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
        result = pf_sd_card_set_owner(PF_SD_CARD_OWNER_USB_MSC);
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
        result = pf_sd_card_set_owner(PF_SD_CARD_OWNER_FIRMWARE);
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
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3au, 0x00u);
    return false;
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16],
                        uint8_t product_rev[4])
{
    (void)lun;
    memcpy(vendor_id, "PicoForge", 8u);
    memcpy(product_id, "SD Card", 7u);
    memcpy(product_rev, "1.0", 3u);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    return media_ready(lun);
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    *block_count = media_ready(lun) ? pf_sd_card_block_count() : 0u;
    *block_size = PF_SD_CARD_BLOCK_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start,
                           bool load_eject)
{
    (void)power_condition;
    if ((lun != 0u) || !msc_enabled) {
        return false;
    }
    if (load_eject) {
        if (start) {
            msc_ejected = false;
        } else {
            if (pf_sd_card_msc_sync() != PF_SD_CARD_OK) {
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
    (void)control;
    if (lun != 0u) {
        return false;
    }
    removal_prevented = prohibit_removal != 0u;
    return true;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    return (lun == 0u) && msc_enabled && !msc_ejected;
}

static bool transfer_is_valid(uint8_t lun, uint32_t lba, uint32_t offset,
                              uint32_t size)
{
    return media_ready(lun) && (lba < pf_sd_card_block_count()) &&
           (offset <= PF_SD_CARD_BLOCK_SIZE) &&
           (size <= PF_SD_CARD_BLOCK_SIZE - offset);
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize)
{
    if ((buffer == NULL) || !transfer_is_valid(lun, lba, offset, bufsize)) {
        return -1;
    }
    if ((offset == 0u) && (bufsize == PF_SD_CARD_BLOCK_SIZE)) {
        return (pf_sd_card_msc_read_blocks(lba, buffer, 1u) == PF_SD_CARD_OK)
            ? (int32_t)bufsize
            : -1;
    }
    if ((pf_sd_card_msc_read_blocks(lba, sector_buffer, 1u) != PF_SD_CARD_OK)) {
        return -1;
    }
    memcpy(buffer, sector_buffer + offset, bufsize);
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize)
{
    if ((buffer == NULL) || !transfer_is_valid(lun, lba, offset, bufsize)) {
        return -1;
    }
    if ((offset == 0u) && (bufsize == PF_SD_CARD_BLOCK_SIZE)) {
        return (pf_sd_card_msc_write_blocks(lba, buffer, 1u) == PF_SD_CARD_OK)
            ? (int32_t)bufsize
            : -1;
    }
    if ((pf_sd_card_msc_read_blocks(lba, sector_buffer, 1u) != PF_SD_CARD_OK)) {
        return -1;
    }
    memcpy(sector_buffer + offset, buffer, bufsize);
    return (pf_sd_card_msc_write_blocks(lba, sector_buffer, 1u) == PF_SD_CARD_OK)
        ? (int32_t)bufsize
        : -1;
}

void tud_msc_write10_complete_cb(uint8_t lun)
{
    if (lun == 0u) {
        (void)pf_sd_card_msc_sync();
    }
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer,
                        uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20u, 0x00u);
    (void)scsi_cmd;
    return -1;
}