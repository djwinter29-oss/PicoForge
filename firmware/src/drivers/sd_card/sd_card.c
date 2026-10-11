#include "sd_card.h"

#include "sdio_pio/sdio_block.h"

static bool transport_selected;
static pf_sd_card_transport_t active_transport;

static pf_sd_card_result_t translate_sdio_result(pf_sdio_pio_result_t result)
{
    switch (result) {
    case PF_SDIO_PIO_OK:
        return PF_SD_CARD_OK;
    case PF_SDIO_PIO_INVALID_ARGUMENT:
        return PF_SD_CARD_INVALID_ARGUMENT;
    case PF_SDIO_PIO_CONFIG_LOCKED:
        return PF_SD_CARD_CONFIG_LOCKED;
    case PF_SDIO_PIO_NOT_CONFIGURED:
        return PF_SD_CARD_NOT_CONFIGURED;
    case PF_SDIO_PIO_CARD_INIT_FAILED:
        return PF_SD_CARD_INIT_FAILED;
    case PF_SDIO_PIO_NOT_READY:
        return PF_SD_CARD_NOT_READY;
    case PF_SDIO_PIO_IO_ERROR:
    default:
        return PF_SD_CARD_IO_ERROR;
    }
}

pf_sd_card_result_t pf_sd_card_init(const pf_sd_card_config_t *config)
{
    pf_sdio_pio_result_t result;

    if (config == NULL) {
        return PF_SD_CARD_INVALID_ARGUMENT;
    }
    if (config->transport == PF_SD_CARD_TRANSPORT_SPI) {
        return PF_SD_CARD_UNSUPPORTED_TRANSPORT;
    }
    if (config->transport != PF_SD_CARD_TRANSPORT_SDIO_PIO) {
        return PF_SD_CARD_INVALID_ARGUMENT;
    }
    if (transport_selected && (active_transport != config->transport)) {
        return PF_SD_CARD_CONFIG_LOCKED;
    }

    result = pf_sdio_pio_init(&config->bus.sdio_pio);
    if (result != PF_SDIO_PIO_INVALID_ARGUMENT) {
        active_transport = config->transport;
        transport_selected = true;
    }
    return translate_sdio_result(result);
}

bool pf_sd_card_is_initialized(void)
{
    return transport_selected &&
           (active_transport == PF_SD_CARD_TRANSPORT_SDIO_PIO) &&
           pf_sdio_pio_is_initialized();
}

uint32_t pf_sd_card_block_count(void)
{
    return pf_sd_card_is_initialized() ? pf_sdio_pio_block_count() : 0u;
}

pf_sd_card_result_t pf_sd_card_read_blocks(uint32_t first_block, void *buffer,
                                           uint32_t block_count)
{
    if (!transport_selected) {
        return PF_SD_CARD_NOT_CONFIGURED;
    }
    if (active_transport != PF_SD_CARD_TRANSPORT_SDIO_PIO) {
        return PF_SD_CARD_NOT_READY;
    }
    return translate_sdio_result(
        pf_sdio_pio_read_blocks(first_block, buffer, block_count));
}

pf_sd_card_result_t pf_sd_card_write_blocks(uint32_t first_block,
                                            const void *buffer,
                                            uint32_t block_count)
{
    if (!transport_selected) {
        return PF_SD_CARD_NOT_CONFIGURED;
    }
    if (active_transport != PF_SD_CARD_TRANSPORT_SDIO_PIO) {
        return PF_SD_CARD_NOT_READY;
    }
    return translate_sdio_result(
        pf_sdio_pio_write_blocks(first_block, buffer, block_count));
}

pf_sd_card_result_t pf_sd_card_sync(void)
{
    if (!transport_selected) {
        return PF_SD_CARD_NOT_CONFIGURED;
    }
    if (active_transport != PF_SD_CARD_TRANSPORT_SDIO_PIO) {
        return PF_SD_CARD_NOT_READY;
    }
    return translate_sdio_result(pf_sdio_pio_sync());
}