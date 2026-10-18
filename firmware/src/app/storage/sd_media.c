/** @file
 * @brief SD application lifecycle, serialized owner gates and failure diagnostics over the raw SD driver.
 */
#include "sd_media.h"

#include <stddef.h>

#include "board/board.h"
#include "drivers/sd_card/sd_port.h"
#include "middleware/storage/fatfs_volume.h"
#include "middleware/storage/sd_block_device.h"
#include "middleware/logging/logging.h"
#include "config/config.h"

#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SPI
#define PF_SELECTED_TRANSPORT PF_SD_TRANSPORT_SPI
#else
#define PF_SELECTED_TRANSPORT PF_SD_TRANSPORT_SDIO_PIO
#endif

static pf_sd_config_t media_config; /**< Board transport borrowed for application lifetime. */
static bool configured; /**< Execution-port and board configuration are available. */
static pf_sd_media_owner_t active_owner = PF_SD_MEDIA_OWNER_FIRMWARE; /**< Exclusive application owner. */
static bool reinit_after_usb_owner; /**< Reinitialize before mounting after a USB release. */
static pf_sd_media_diagnostics_t last_diagnostics; /**< Retained failure and owner snapshot. */
static uint32_t diagnostic_sequence; /**< Monotonic failure counter, preserved by clear. */

static pf_sd_result_t record_failure(pf_sd_media_operation_t operation, pf_sd_result_t result);

pf_sd_result_t pf_sd_media_setup(void)
{
    if (configured) {
        return PF_SD_OK;
    }
    if (!pf_board_get_sd_card_config(&media_config)) {
        PF_LOG_WARNING("sd", "SD board configuration unavailable");
        return PF_SD_NOT_CONFIGURED;
    }
    if (!pf_sd_port_init(&media_config)) {
        PF_LOG_ERROR("sd", "SD execution-port initialization failed");
        return PF_SD_IO_ERROR;
    }
    configured = true;
    return PF_SD_OK;
}

pf_sd_result_t pf_sd_media_initialize(void)
{
    pf_sd_lock();
    pf_sd_result_t result;
    if (!configured) {
        result = PF_SD_NOT_CONFIGURED;
    } else if (active_owner != PF_SD_MEDIA_OWNER_FIRMWARE || pf_fatfs_volume_is_mounted()) {
        result = PF_SD_NOT_READY;
    } else {
        result = pf_sd_init(&media_config);
    }
    result = record_failure(PF_SD_MEDIA_OPERATION_INIT, result);
    pf_sd_unlock();
    return result;
}

/** @brief Retain one failure with owner and driver diagnostics; successful I/O never clears it. */
static pf_sd_result_t record_failure(pf_sd_media_operation_t operation, pf_sd_result_t result)
{
    if (result == PF_SD_OK) {
        return result;
    }
    ++diagnostic_sequence;
    last_diagnostics = (pf_sd_media_diagnostics_t) {
        .sequence = diagnostic_sequence,
        .operation = operation,
        .result = result,
        .owner = active_owner,
        .transport = PF_SELECTED_TRANSPORT,
        .transport_init_stage = pf_sd_get_init_stage(),
    };
    pf_sd_get_profile(&last_diagnostics.profile);
    return result;
}

void pf_sd_media_get_diagnostics(pf_sd_media_diagnostics_t *diagnostics)
{
    pf_sd_lock();
    if (diagnostics != NULL) {
        *diagnostics = last_diagnostics;
    }
    pf_sd_unlock();
}

void pf_sd_media_clear_diagnostics(void)
{
    pf_sd_lock();
    last_diagnostics = (pf_sd_media_diagnostics_t) {
        .sequence = diagnostic_sequence,
        .operation = PF_SD_MEDIA_OPERATION_NONE,
        .result = PF_SD_OK,
        .owner = active_owner,
        .transport = PF_SELECTED_TRANSPORT,
    };
    pf_sd_unlock();
}

pf_sd_media_owner_t pf_sd_media_get_owner(void)
{
    pf_sd_lock();
    pf_sd_media_owner_t result = active_owner;
    pf_sd_unlock();
    return result;
}

bool pf_sd_media_take_reinit_request(void)
{
    pf_sd_lock();
    bool requested = reinit_after_usb_owner;
    reinit_after_usb_owner = false;
    pf_sd_unlock();
    return requested;
}

/** @brief Keep the owner identity stable throughout one synchronous read batch. */
static pf_sd_result_t read_blocks_for_owner(pf_sd_media_owner_t owner,
                                           uint32_t first_block, void *buffer, uint32_t block_count)
{
    pf_sd_lock();
    if (active_owner != owner) {
        pf_sd_result_t result =
            record_failure(PF_SD_MEDIA_OPERATION_READ, PF_SD_NOT_READY);
        pf_sd_unlock();
        return result;
    }
    pf_sd_result_t result = pf_sd_read_blocks(first_block, buffer, block_count);
    result = record_failure(PF_SD_MEDIA_OPERATION_READ, result);
    pf_sd_unlock();
    return result;
}

pf_sd_result_t pf_sd_media_read_blocks(uint32_t first_block, void *buffer, uint32_t block_count)
{
    return read_blocks_for_owner(PF_SD_MEDIA_OWNER_FIRMWARE, first_block, buffer, block_count);
}

/** @brief Keep the owner identity stable throughout one synchronous write batch; never retry. */
static pf_sd_result_t write_blocks_for_owner(pf_sd_media_owner_t owner,
                                            uint32_t first_block, const void *buffer, uint32_t block_count)
{
    pf_sd_lock();
    if (active_owner != owner) {
        pf_sd_result_t result =
            record_failure(PF_SD_MEDIA_OPERATION_WRITE, PF_SD_NOT_READY);
        pf_sd_unlock();
        return result;
    }
    pf_sd_result_t result = pf_sd_write_blocks(first_block, buffer, block_count);
    result = record_failure(PF_SD_MEDIA_OPERATION_WRITE, result);
    pf_sd_unlock();
    return result;
}

pf_sd_result_t pf_sd_media_write_blocks(uint32_t first_block, const void *buffer, uint32_t block_count)
{
    return write_blocks_for_owner(PF_SD_MEDIA_OWNER_FIRMWARE, first_block, buffer, block_count);
}

/** @brief Sync only for the current owner, never converting a failed flush into success. */
static pf_sd_result_t sync_for_owner(pf_sd_media_owner_t owner)
{
    pf_sd_lock();
    if (active_owner != owner) {
        pf_sd_result_t result =
            record_failure(PF_SD_MEDIA_OPERATION_SYNC, PF_SD_NOT_READY);
        pf_sd_unlock();
        return result;
    }
    pf_sd_result_t result = pf_sd_sync();
    result = record_failure(PF_SD_MEDIA_OPERATION_SYNC, result);
    pf_sd_unlock();
    return result;
}

pf_sd_result_t pf_sd_media_sync(void)
{
    return sync_for_owner(PF_SD_MEDIA_OWNER_FIRMWARE);
}

pf_sd_result_t pf_sd_media_acquire_usb(void)
{
    pf_sd_lock();
    if (active_owner == PF_SD_MEDIA_OWNER_USB_MSC) {
        pf_sd_unlock();
        return PF_SD_OK;
    }
    if (!pf_sd_is_initialized() || pf_fatfs_volume_is_mounted()) {
        pf_sd_result_t result =
            record_failure(PF_SD_MEDIA_OPERATION_MSC_ACQUIRE, PF_SD_NOT_READY);
        pf_sd_unlock();
        return result;
    }
    active_owner = PF_SD_MEDIA_OWNER_USB_MSC;
    pf_sd_unlock();
    return PF_SD_OK;
}

pf_sd_result_t pf_sd_media_release_usb(void)
{
    pf_sd_lock();
    if (active_owner == PF_SD_MEDIA_OWNER_USB_MSC) {
        reinit_after_usb_owner = true;
        active_owner = PF_SD_MEDIA_OWNER_FIRMWARE;
    }
    pf_sd_unlock();
    return PF_SD_OK;
}

pf_sd_result_t pf_sd_media_release_usb_for_recovery(void)
{
    pf_sd_lock();
    pf_sd_result_t result = active_owner == PF_SD_MEDIA_OWNER_USB_MSC && !pf_sd_is_initialized()
        ? pf_sd_media_release_usb() : PF_SD_NOT_READY;
    pf_sd_unlock();
    return result;
}

pf_sd_result_t pf_sd_media_usb_read_blocks(uint32_t first_block, void *buffer, uint32_t block_count)
{
    return read_blocks_for_owner(PF_SD_MEDIA_OWNER_USB_MSC, first_block, buffer, block_count);
}

pf_sd_result_t pf_sd_media_usb_write_blocks(uint32_t first_block, const void *buffer, uint32_t block_count)
{
    return write_blocks_for_owner(PF_SD_MEDIA_OWNER_USB_MSC, first_block, buffer, block_count);
}

pf_sd_result_t pf_sd_media_usb_sync(void)
{
    return sync_for_owner(PF_SD_MEDIA_OWNER_USB_MSC);
}

/** @brief Map protocol errors without relying on enum numeric equivalence. */
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

/** @brief Query raw geometry under the reservation; metadata never authorizes I/O. */
static pf_block_result_t media_geometry(void *context, pf_block_geometry_t *value)
{
    (void)context;
    return pf_block_geometry(pf_sd_block_device(), value);
}

static pf_block_result_t media_read(void *context, uint32_t first, void *buffer, uint32_t count)
{
    (void)context;
    return block_result(pf_sd_media_read_blocks(first, buffer, count));
}

static pf_block_result_t media_write(void *context, uint32_t first, const void *buffer, uint32_t count)
{
    (void)context;
    return block_result(pf_sd_media_write_blocks(first, buffer, count));
}

static pf_block_result_t media_sync(void *context)
{
    (void)context;
    return block_result(pf_sd_media_sync());
}

const pf_block_device_t *pf_sd_media_firmware_device(void)
{
    static const pf_block_device_ops_t ops = {
        .geometry = media_geometry, .read = media_read, .write = media_write, .sync = media_sync,
    };
    static const pf_block_device_t device = {.ops = &ops};
    return &device;
}
