#include "sd_card.h"
#include "sd_card_msc.h"
#include "sd_card_port.h"

#include <stddef.h>

bool pf_sd_card_try_lock(void)
{
    return pf_sd_card_port_try_lock();
}

void pf_sd_card_lock(void)
{
    pf_sd_card_port_lock();
}

void pf_sd_card_unlock(void)
{
    pf_sd_card_port_unlock();
}

#include "config/config.h"
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SPI
#include "spi/sd_spi.h"
#define PF_SELECTED_TRANSPORT PF_SD_CARD_TRANSPORT_SPI
#define pf_selected_init(config) pf_sd_spi_init(&(config)->bus.spi)
#define pf_selected_is_initialized() pf_sd_spi_is_initialized()
#define pf_selected_block_count() pf_sd_spi_block_count()
#define pf_selected_read(first, buffer, count) pf_sd_spi_read_blocks((first), (buffer), (count))
#define pf_selected_write(first, buffer, count) pf_sd_spi_write_blocks((first), (buffer), (count))
#define pf_selected_sync() pf_sd_spi_sync()
#define pf_selected_get_info(info) pf_sd_spi_get_info(info)
#define pf_selected_get_init_stage() ((uint32_t)pf_sd_spi_get_init_stage())
#define PF_SELECTED_TRANSPORT_NAME "spi"
#else
#include "sdio_pio/sdio_pio.h"
#define PF_SELECTED_TRANSPORT PF_SD_CARD_TRANSPORT_SDIO_PIO
#define pf_selected_init(config) pf_sdio_pio_init(&(config)->bus.sdio_pio)
#define pf_selected_is_initialized() pf_sdio_pio_is_initialized()
#define pf_selected_block_count() pf_sdio_pio_block_count()
#define pf_selected_read(first, buffer, count) pf_sdio_pio_read_blocks((first), (buffer), (count))
#define pf_selected_write(first, buffer, count) pf_sdio_pio_write_blocks((first), (buffer), (count))
#define pf_selected_sync() pf_sdio_pio_sync()
#define pf_selected_get_info(info) pf_sdio_pio_get_info(info)
#define pf_selected_get_init_stage() ((uint32_t)pf_sdio_pio_get_init_stage())
#define PF_SELECTED_TRANSPORT_NAME "sdio"
#endif

static bool transport_selected;
static pf_sd_card_owner_t active_owner = PF_SD_CARD_OWNER_FIRMWARE;
static bool reinit_after_usb_owner;
static pf_sd_card_diagnostics_t last_diagnostics;
static uint32_t diagnostic_sequence;

static pf_sd_card_result_t record_failure(pf_sd_card_operation_t operation,
                                          pf_sd_card_result_t result)
{
    if (result == PF_SD_CARD_OK) {
        return result;
    }
    ++diagnostic_sequence;
    last_diagnostics = (pf_sd_card_diagnostics_t) {
        .sequence = diagnostic_sequence,
        .operation = operation,
        .result = result,
        .owner = active_owner,
        .transport = PF_SELECTED_TRANSPORT,
        .transport_init_stage = transport_selected ? pf_selected_get_init_stage() : 0u,
    };
    pf_sd_card_get_profile(&last_diagnostics.profile);
    return result;
}

void pf_sd_card_get_diagnostics(pf_sd_card_diagnostics_t *diagnostics)
{
    pf_sd_card_lock();
    if (diagnostics != NULL) {
        *diagnostics = last_diagnostics;
    }
    pf_sd_card_unlock();
}

void pf_sd_card_clear_diagnostics(void)
{
    pf_sd_card_lock();
    last_diagnostics = (pf_sd_card_diagnostics_t) {
        .sequence = diagnostic_sequence,
        .operation = PF_SD_CARD_OPERATION_NONE,
        .result = PF_SD_CARD_OK,
        .owner = active_owner,
        .transport = PF_SELECTED_TRANSPORT,
    };
    pf_sd_card_unlock();
}

pf_sd_card_owner_t pf_sd_card_get_owner(void)
{
    pf_sd_card_lock();
    pf_sd_card_owner_t result = active_owner;
    pf_sd_card_unlock();
    return result;
}

bool pf_sd_card_take_reinit_request(void)
{
    pf_sd_card_lock();
    bool requested = reinit_after_usb_owner;
    reinit_after_usb_owner = false;
    pf_sd_card_unlock();
    return requested;
}

pf_sd_card_result_t pf_sd_card_init(const pf_sd_card_config_t *config)
{
    pf_sd_card_lock();
    if ((config == NULL) || (config->transport != PF_SELECTED_TRANSPORT)) {
        pf_sd_card_result_t result =
            record_failure(PF_SD_CARD_OPERATION_INIT, PF_SD_CARD_INVALID_ARGUMENT);
        pf_sd_card_unlock();
        return result;
    }
    if (active_owner != PF_SD_CARD_OWNER_FIRMWARE) {
        pf_sd_card_result_t result =
            record_failure(PF_SD_CARD_OPERATION_INIT, PF_SD_CARD_NOT_READY);
        pf_sd_card_unlock();
        return result;
    }
    pf_sd_card_result_t result = pf_selected_init(config);
    if (result != PF_SD_CARD_INVALID_ARGUMENT) {
        transport_selected = true;
    }
    result = record_failure(PF_SD_CARD_OPERATION_INIT, result);
    pf_sd_card_unlock();
    return result;
}

bool pf_sd_card_is_initialized(void)
{
    pf_sd_card_lock();
    if (!transport_selected) {
        pf_sd_card_unlock();
        return false;
    }
    bool result = pf_selected_is_initialized();
    pf_sd_card_unlock();
    return result;
}

uint32_t pf_sd_card_block_count(void)
{
    pf_sd_card_lock();
    if (!pf_sd_card_is_initialized()) {
        pf_sd_card_unlock();
        return 0u;
    }
    uint32_t result = pf_selected_block_count();
    pf_sd_card_unlock();
    return result;
}

static pf_sd_card_result_t read_blocks_for_owner(pf_sd_card_owner_t owner,
                                                uint32_t first_block, void *buffer,
                                                uint32_t block_count)
{
    pf_sd_card_lock();
    if (active_owner != owner) {
        pf_sd_card_result_t result =
            record_failure(PF_SD_CARD_OPERATION_READ, PF_SD_CARD_NOT_READY);
        pf_sd_card_unlock();
        return result;
    }
    if (!transport_selected) {
        pf_sd_card_result_t result =
            record_failure(PF_SD_CARD_OPERATION_READ, PF_SD_CARD_NOT_CONFIGURED);
        pf_sd_card_unlock();
        return result;
    }
    pf_sd_card_result_t result = pf_selected_read(first_block, buffer, block_count);
    result = record_failure(PF_SD_CARD_OPERATION_READ, result);
    pf_sd_card_unlock();
    return result;
}

pf_sd_card_result_t pf_sd_card_read_blocks(uint32_t first_block, void *buffer,
                                           uint32_t block_count)
{
    return read_blocks_for_owner(PF_SD_CARD_OWNER_FIRMWARE, first_block, buffer, block_count);
}

static pf_sd_card_result_t write_blocks_for_owner(pf_sd_card_owner_t owner,
                                                 uint32_t first_block, const void *buffer,
                                                 uint32_t block_count)
{
    pf_sd_card_lock();
    if (active_owner != owner) {
        pf_sd_card_result_t result =
            record_failure(PF_SD_CARD_OPERATION_WRITE, PF_SD_CARD_NOT_READY);
        pf_sd_card_unlock();
        return result;
    }
    if (!transport_selected) {
        pf_sd_card_result_t result =
            record_failure(PF_SD_CARD_OPERATION_WRITE, PF_SD_CARD_NOT_CONFIGURED);
        pf_sd_card_unlock();
        return result;
    }
    pf_sd_card_result_t result = pf_selected_write(first_block, buffer, block_count);
    result = record_failure(PF_SD_CARD_OPERATION_WRITE, result);
    pf_sd_card_unlock();
    return result;
}

pf_sd_card_result_t pf_sd_card_write_blocks(uint32_t first_block,
                                            const void *buffer,
                                            uint32_t block_count)
{
    return write_blocks_for_owner(PF_SD_CARD_OWNER_FIRMWARE, first_block, buffer, block_count);
}

static pf_sd_card_result_t sync_for_owner(pf_sd_card_owner_t owner)
{
    pf_sd_card_lock();
    if (active_owner != owner) {
        pf_sd_card_result_t result =
            record_failure(PF_SD_CARD_OPERATION_SYNC, PF_SD_CARD_NOT_READY);
        pf_sd_card_unlock();
        return result;
    }
    if (!transport_selected) {
        pf_sd_card_result_t result =
            record_failure(PF_SD_CARD_OPERATION_SYNC, PF_SD_CARD_NOT_CONFIGURED);
        pf_sd_card_unlock();
        return result;
    }
    pf_sd_card_result_t result = pf_selected_sync();
    result = record_failure(PF_SD_CARD_OPERATION_SYNC, result);
    pf_sd_card_unlock();
    return result;
}

pf_sd_card_result_t pf_sd_card_sync(void)
{
    return sync_for_owner(PF_SD_CARD_OWNER_FIRMWARE);
}

pf_sd_card_result_t pf_sd_card_msc_acquire(void)
{
    pf_sd_card_lock();
    if (active_owner == PF_SD_CARD_OWNER_USB_MSC) {
        pf_sd_card_unlock();
        return PF_SD_CARD_OK;
    }
    if (!pf_sd_card_is_initialized()) {
        pf_sd_card_result_t result =
            record_failure(PF_SD_CARD_OPERATION_MSC_ACQUIRE, PF_SD_CARD_NOT_READY);
        pf_sd_card_unlock();
        return result;
    }
    active_owner = PF_SD_CARD_OWNER_USB_MSC;
    pf_sd_card_unlock();
    return PF_SD_CARD_OK;
}

pf_sd_card_result_t pf_sd_card_msc_release(void)
{
    pf_sd_card_lock();
    if (active_owner == PF_SD_CARD_OWNER_USB_MSC) {
        reinit_after_usb_owner = true;
        active_owner = PF_SD_CARD_OWNER_FIRMWARE;
    }
    pf_sd_card_unlock();
    return PF_SD_CARD_OK;
}

pf_sd_card_result_t pf_sd_card_msc_read_blocks(uint32_t first_block, void *buffer,
                                               uint32_t block_count)
{
    return read_blocks_for_owner(PF_SD_CARD_OWNER_USB_MSC, first_block, buffer, block_count);
}

pf_sd_card_result_t pf_sd_card_msc_write_blocks(uint32_t first_block,
                                                const void *buffer,
                                                uint32_t block_count)
{
    return write_blocks_for_owner(PF_SD_CARD_OWNER_USB_MSC, first_block, buffer, block_count);
}

pf_sd_card_result_t pf_sd_card_msc_sync(void)
{
    return sync_for_owner(PF_SD_CARD_OWNER_USB_MSC);
}

void pf_sd_card_get_info(pf_sd_card_info_t *info)
{
    pf_sd_card_lock();
    if (info == NULL) {
        pf_sd_card_unlock();
        return;
    }
    *info = (pf_sd_card_info_t){ 0 };
    if (!transport_selected) {
        pf_sd_card_unlock();
        return;
    }
    pf_selected_get_info(info);
    pf_sd_card_unlock();
}

const char *pf_sd_card_transport_name(void)
{
    pf_sd_card_lock();
    if (!transport_selected) {
        pf_sd_card_unlock();
        return "unconfigured";
    }
    pf_sd_card_unlock();
    return PF_SELECTED_TRANSPORT_NAME;
}

void pf_sd_card_confirm_removed(void)
{
    pf_sd_card_lock();
    if (!transport_selected) {
        pf_sd_card_unlock();
        return;
    }
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SPI
    /* SPI operations are per-call; there is no persistent stream to abandon. */
#else
    pf_sdio_pio_confirm_card_removed();
#endif
    pf_sd_card_unlock();
}
