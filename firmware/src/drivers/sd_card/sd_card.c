#include "sd_card.h"

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
#define PF_SELECTED_TRANSPORT_NAME "sdio"
#endif

static bool transport_selected;
static pf_sd_card_owner_t active_owner = PF_SD_CARD_OWNER_FIRMWARE;
static bool reinit_after_usb_owner;

pf_sd_card_owner_t pf_sd_card_get_owner(void)
{
    return active_owner;
}

bool pf_sd_card_take_reinit_request(void)
{
    bool requested = reinit_after_usb_owner;
    reinit_after_usb_owner = false;
    return requested;
}

pf_sd_card_result_t pf_sd_card_init(const pf_sd_card_config_t *config)
{
    if ((config == NULL) || (config->transport != PF_SELECTED_TRANSPORT)) {
        return PF_SD_CARD_INVALID_ARGUMENT;
    }
    if (active_owner != PF_SD_CARD_OWNER_FIRMWARE) {
        return PF_SD_CARD_NOT_READY;
    }
    pf_sd_card_result_t result = pf_selected_init(config);
    if (result != PF_SD_CARD_INVALID_ARGUMENT) {
        transport_selected = true;
    }
    return result;
}

bool pf_sd_card_is_initialized(void)
{
    if (!transport_selected) {
        return false;
    }
    return pf_selected_is_initialized();
}

uint32_t pf_sd_card_block_count(void)
{
    if (!pf_sd_card_is_initialized()) {
        return 0u;
    }
    return pf_selected_block_count();
}

pf_sd_card_result_t pf_sd_card_read_blocks(uint32_t first_block, void *buffer,
                                           uint32_t block_count)
{
    if (active_owner != PF_SD_CARD_OWNER_FIRMWARE) {
        return PF_SD_CARD_NOT_READY;
    }
    if (!transport_selected) {
        return PF_SD_CARD_NOT_CONFIGURED;
    }
    return pf_selected_read(first_block, buffer, block_count);
}

pf_sd_card_result_t pf_sd_card_write_blocks(uint32_t first_block,
                                            const void *buffer,
                                            uint32_t block_count)
{
    if (active_owner != PF_SD_CARD_OWNER_FIRMWARE) {
        return PF_SD_CARD_NOT_READY;
    }
    if (!transport_selected) {
        return PF_SD_CARD_NOT_CONFIGURED;
    }
    return pf_selected_write(first_block, buffer, block_count);
}

pf_sd_card_result_t pf_sd_card_sync(void)
{
    if (active_owner != PF_SD_CARD_OWNER_FIRMWARE) {
        return PF_SD_CARD_NOT_READY;
    }
    if (!transport_selected) {
        return PF_SD_CARD_NOT_CONFIGURED;
    }
    return pf_selected_sync();
}

pf_sd_card_result_t pf_sd_card_set_owner(pf_sd_card_owner_t owner)
{
    if ((owner != PF_SD_CARD_OWNER_FIRMWARE) && (owner != PF_SD_CARD_OWNER_USB_MSC)) {
        return PF_SD_CARD_INVALID_ARGUMENT;
    }
    if ((owner == PF_SD_CARD_OWNER_USB_MSC) && !pf_sd_card_is_initialized()) {
        return PF_SD_CARD_NOT_READY;
    }
    if ((active_owner == PF_SD_CARD_OWNER_USB_MSC) &&
        (owner == PF_SD_CARD_OWNER_FIRMWARE)) {
        reinit_after_usb_owner = true;
    }
    active_owner = owner;
    return PF_SD_CARD_OK;
}

pf_sd_card_result_t pf_sd_card_msc_read_blocks(uint32_t first_block, void *buffer,
                                               uint32_t block_count)
{
    if (active_owner != PF_SD_CARD_OWNER_USB_MSC) {
        return PF_SD_CARD_NOT_READY;
    }
    if (!transport_selected) {
        return PF_SD_CARD_NOT_CONFIGURED;
    }
    return pf_selected_read(first_block, buffer, block_count);
}

pf_sd_card_result_t pf_sd_card_msc_write_blocks(uint32_t first_block,
                                                const void *buffer,
                                                uint32_t block_count)
{
    if (active_owner != PF_SD_CARD_OWNER_USB_MSC) {
        return PF_SD_CARD_NOT_READY;
    }
    if (!transport_selected) {
        return PF_SD_CARD_NOT_CONFIGURED;
    }
    return pf_selected_write(first_block, buffer, block_count);
}

pf_sd_card_result_t pf_sd_card_msc_sync(void)
{
    if (active_owner != PF_SD_CARD_OWNER_USB_MSC) {
        return PF_SD_CARD_NOT_READY;
    }
    if (!transport_selected) {
        return PF_SD_CARD_NOT_CONFIGURED;
    }
    return pf_selected_sync();
}

void pf_sd_card_get_info(pf_sd_card_info_t *info)
{
    if (info == NULL) {
        return;
    }
    *info = (pf_sd_card_info_t){ 0 };
    if (!transport_selected) {
        return;
    }
    pf_selected_get_info(info);
}

const char *pf_sd_card_transport_name(void)
{
    if (!transport_selected) {
        return "unconfigured";
    }
    return PF_SELECTED_TRANSPORT_NAME;
}