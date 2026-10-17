#include "sd.h"
#include "sd_msc.h"
#include "sd_port.h"

#include <stddef.h>

bool pf_sd_try_lock(void)
{
    return pf_sd_port_try_lock();
}

void pf_sd_lock(void)
{
    pf_sd_port_lock();
}

void pf_sd_unlock(void)
{
    pf_sd_port_unlock();
}

#include "config/config.h"
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SPI
#include "sd_spi.h"
#define PF_SELECTED_TRANSPORT PF_SD_TRANSPORT_SPI
#define pf_selected_init(config) pf_sd_spi_init(config)
#define PF_SELECTED_MODE PF_STORAGE_BUS_SPI
#define pf_selected_is_initialized() pf_sd_spi_is_initialized()
#define pf_selected_block_count() pf_sd_spi_block_count()
#define pf_selected_read(first, buffer, count) pf_sd_spi_read_blocks((first), (buffer), (count))
#define pf_selected_write(first, buffer, count) pf_sd_spi_write_blocks((first), (buffer), (count))
#define pf_selected_sync() pf_sd_spi_sync()
#define pf_selected_get_info(info) pf_sd_spi_get_info(info)
#define pf_selected_get_init_stage() ((uint32_t)pf_sd_spi_get_init_stage())
#define PF_SELECTED_TRANSPORT_NAME "spi"
#else
#include "sd_native.h"
#define PF_SELECTED_TRANSPORT PF_SD_TRANSPORT_SDIO_PIO
#define pf_selected_init(config) pf_sd_native_init(config)
#define PF_SELECTED_MODE PF_STORAGE_BUS_NATIVE_SD
#define pf_selected_is_initialized() pf_sd_native_is_initialized()
#define pf_selected_block_count() pf_sd_native_block_count()
#define pf_selected_read(first, buffer, count) pf_sd_native_read_blocks((first), (buffer), (count))
#define pf_selected_write(first, buffer, count) pf_sd_native_write_blocks((first), (buffer), (count))
#define pf_selected_sync() pf_sd_native_sync()
#define pf_selected_get_info(info) pf_sd_native_get_info(info)
#define pf_selected_get_init_stage() ((uint32_t)pf_sd_native_get_init_stage())
#define PF_SELECTED_TRANSPORT_NAME "sdio"
#endif

static bool transport_selected;
static pf_sd_owner_t active_owner = PF_SD_OWNER_FIRMWARE;
static bool reinit_after_usb_owner;
static pf_sd_diagnostics_t last_diagnostics;
static uint32_t diagnostic_sequence;

static pf_sd_result_t record_failure(pf_sd_operation_t operation,
                                          pf_sd_result_t result)
{
    if (result == PF_SD_OK) {
        return result;
    }
    ++diagnostic_sequence;
    last_diagnostics = (pf_sd_diagnostics_t) {
        .sequence = diagnostic_sequence,
        .operation = operation,
        .result = result,
        .owner = active_owner,
        .transport = PF_SELECTED_TRANSPORT,
        .transport_init_stage = transport_selected ? pf_selected_get_init_stage() : 0u,
    };
    pf_sd_get_profile(&last_diagnostics.profile);
    return result;
}

void pf_sd_get_diagnostics(pf_sd_diagnostics_t *diagnostics)
{
    pf_sd_lock();
    if (diagnostics != NULL) {
        *diagnostics = last_diagnostics;
    }
    pf_sd_unlock();
}

void pf_sd_clear_diagnostics(void)
{
    pf_sd_lock();
    last_diagnostics = (pf_sd_diagnostics_t) {
        .sequence = diagnostic_sequence,
        .operation = PF_SD_OPERATION_NONE,
        .result = PF_SD_OK,
        .owner = active_owner,
        .transport = PF_SELECTED_TRANSPORT,
    };
    pf_sd_unlock();
}

pf_sd_owner_t pf_sd_get_owner(void)
{
    pf_sd_lock();
    pf_sd_owner_t result = active_owner;
    pf_sd_unlock();
    return result;
}

bool pf_sd_take_reinit_request(void)
{
    pf_sd_lock();
    bool requested = reinit_after_usb_owner;
    reinit_after_usb_owner = false;
    pf_sd_unlock();
    return requested;
}

pf_sd_result_t pf_sd_init(const pf_sd_config_t *config)
{
    pf_sd_lock();
    if ((config == NULL) || (config->transport.mode != PF_SELECTED_MODE)) {
        pf_sd_result_t result =
            record_failure(PF_SD_OPERATION_INIT, PF_SD_INVALID_ARGUMENT);
        pf_sd_unlock();
        return result;
    }
    if (active_owner != PF_SD_OWNER_FIRMWARE) {
        pf_sd_result_t result =
            record_failure(PF_SD_OPERATION_INIT, PF_SD_NOT_READY);
        pf_sd_unlock();
        return result;
    }
    pf_sd_result_t result = pf_selected_init(config);
    if (result != PF_SD_INVALID_ARGUMENT) {
        transport_selected = true;
    }
    result = record_failure(PF_SD_OPERATION_INIT, result);
    pf_sd_unlock();
    return result;
}

bool pf_sd_is_initialized(void)
{
    pf_sd_lock();
    if (!transport_selected) {
        pf_sd_unlock();
        return false;
    }
    bool result = pf_selected_is_initialized();
    pf_sd_unlock();
    return result;
}

uint32_t pf_sd_block_count(void)
{
    pf_sd_lock();
    if (!pf_sd_is_initialized()) {
        pf_sd_unlock();
        return 0u;
    }
    uint32_t result = pf_selected_block_count();
    pf_sd_unlock();
    return result;
}

static pf_sd_result_t read_blocks_for_owner(pf_sd_owner_t owner,
                                                uint32_t first_block, void *buffer,
                                                uint32_t block_count)
{
    pf_sd_lock();
    if (active_owner != owner) {
        pf_sd_result_t result =
            record_failure(PF_SD_OPERATION_READ, PF_SD_NOT_READY);
        pf_sd_unlock();
        return result;
    }
    if (!transport_selected) {
        pf_sd_result_t result =
            record_failure(PF_SD_OPERATION_READ, PF_SD_NOT_CONFIGURED);
        pf_sd_unlock();
        return result;
    }
    pf_sd_result_t result = pf_selected_read(first_block, buffer, block_count);
    result = record_failure(PF_SD_OPERATION_READ, result);
    pf_sd_unlock();
    return result;
}

pf_sd_result_t pf_sd_read_blocks(uint32_t first_block, void *buffer,
                                           uint32_t block_count)
{
    return read_blocks_for_owner(PF_SD_OWNER_FIRMWARE, first_block, buffer, block_count);
}

static pf_sd_result_t write_blocks_for_owner(pf_sd_owner_t owner,
                                                 uint32_t first_block, const void *buffer,
                                                 uint32_t block_count)
{
    pf_sd_lock();
    if (active_owner != owner) {
        pf_sd_result_t result =
            record_failure(PF_SD_OPERATION_WRITE, PF_SD_NOT_READY);
        pf_sd_unlock();
        return result;
    }
    if (!transport_selected) {
        pf_sd_result_t result =
            record_failure(PF_SD_OPERATION_WRITE, PF_SD_NOT_CONFIGURED);
        pf_sd_unlock();
        return result;
    }
    pf_sd_result_t result = pf_selected_write(first_block, buffer, block_count);
    result = record_failure(PF_SD_OPERATION_WRITE, result);
    pf_sd_unlock();
    return result;
}

pf_sd_result_t pf_sd_write_blocks(uint32_t first_block,
                                            const void *buffer,
                                            uint32_t block_count)
{
    return write_blocks_for_owner(PF_SD_OWNER_FIRMWARE, first_block, buffer, block_count);
}

static pf_sd_result_t sync_for_owner(pf_sd_owner_t owner)
{
    pf_sd_lock();
    if (active_owner != owner) {
        pf_sd_result_t result =
            record_failure(PF_SD_OPERATION_SYNC, PF_SD_NOT_READY);
        pf_sd_unlock();
        return result;
    }
    if (!transport_selected) {
        pf_sd_result_t result =
            record_failure(PF_SD_OPERATION_SYNC, PF_SD_NOT_CONFIGURED);
        pf_sd_unlock();
        return result;
    }
    pf_sd_result_t result = pf_selected_sync();
    result = record_failure(PF_SD_OPERATION_SYNC, result);
    pf_sd_unlock();
    return result;
}

pf_sd_result_t pf_sd_sync(void)
{
    return sync_for_owner(PF_SD_OWNER_FIRMWARE);
}

pf_sd_result_t pf_sd_msc_acquire(void)
{
    pf_sd_lock();
    if (active_owner == PF_SD_OWNER_USB_MSC) {
        pf_sd_unlock();
        return PF_SD_OK;
    }
    if (!pf_sd_is_initialized()) {
        pf_sd_result_t result =
            record_failure(PF_SD_OPERATION_MSC_ACQUIRE, PF_SD_NOT_READY);
        pf_sd_unlock();
        return result;
    }
    active_owner = PF_SD_OWNER_USB_MSC;
    pf_sd_unlock();
    return PF_SD_OK;
}

pf_sd_result_t pf_sd_msc_release(void)
{
    pf_sd_lock();
    if (active_owner == PF_SD_OWNER_USB_MSC) {
        reinit_after_usb_owner = true;
        active_owner = PF_SD_OWNER_FIRMWARE;
    }
    pf_sd_unlock();
    return PF_SD_OK;
}

pf_sd_result_t pf_sd_msc_read_blocks(uint32_t first_block, void *buffer,
                                               uint32_t block_count)
{
    return read_blocks_for_owner(PF_SD_OWNER_USB_MSC, first_block, buffer, block_count);
}

pf_sd_result_t pf_sd_msc_write_blocks(uint32_t first_block,
                                                const void *buffer,
                                                uint32_t block_count)
{
    return write_blocks_for_owner(PF_SD_OWNER_USB_MSC, first_block, buffer, block_count);
}

pf_sd_result_t pf_sd_msc_sync(void)
{
    return sync_for_owner(PF_SD_OWNER_USB_MSC);
}

void pf_sd_get_info(pf_sd_info_t *info)
{
    pf_sd_lock();
    if (info == NULL) {
        pf_sd_unlock();
        return;
    }
    *info = (pf_sd_info_t){ 0 };
    if (!transport_selected) {
        pf_sd_unlock();
        return;
    }
    pf_selected_get_info(info);
    pf_sd_unlock();
}

const char *pf_sd_transport_name(void)
{
    pf_sd_lock();
    if (!transport_selected) {
        pf_sd_unlock();
        return "unconfigured";
    }
    pf_sd_unlock();
    return PF_SELECTED_TRANSPORT_NAME;
}

void pf_sd_confirm_removed(void)
{
    pf_sd_lock();
    if (!transport_selected) {
        pf_sd_unlock();
        return;
    }
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SPI
    /* SPI operations are per-call; there is no persistent stream to abandon. */
#else
    pf_sd_native_confirm_card_removed();
#endif
    pf_sd_unlock();
}
