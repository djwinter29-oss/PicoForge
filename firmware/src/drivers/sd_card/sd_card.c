#include "sd_card.h"

#include "sdio_pio/sdio_block.h"
#include "spi/sdspi.h"

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
    pf_sd_card_result_t result;

    if (config == NULL) {
        return PF_SD_CARD_INVALID_ARGUMENT;
    }
    if (config->transport == PF_SD_CARD_TRANSPORT_SPI) {
        if (transport_selected && (active_transport != config->transport)) {
            return PF_SD_CARD_CONFIG_LOCKED;
        }
        result = pf_sdspi_init(&config->bus.spi);
    } else if (config->transport == PF_SD_CARD_TRANSPORT_SDIO_PIO) {
        pf_sdio_pio_result_t sdio_result;

        if (transport_selected && (active_transport != config->transport)) {
            return PF_SD_CARD_CONFIG_LOCKED;
        }
        sdio_result = pf_sdio_pio_init(&config->bus.sdio_pio);
        result = translate_sdio_result(sdio_result);
    } else {
        return PF_SD_CARD_INVALID_ARGUMENT;
    }
    if (result != PF_SD_CARD_INVALID_ARGUMENT) {
        active_transport = config->transport;
        transport_selected = true;
    }
    return result;
}

bool pf_sd_card_is_initialized(void)
{
    if (!transport_selected) {
        return false;
    }
    if (active_transport == PF_SD_CARD_TRANSPORT_SPI) {
        return pf_sdspi_is_initialized();
    }
    return pf_sdio_pio_is_initialized();
}

uint32_t pf_sd_card_block_count(void)
{
    if (!pf_sd_card_is_initialized()) {
        return 0u;
    }
    return (active_transport == PF_SD_CARD_TRANSPORT_SPI)
        ? pf_sdspi_block_count()
        : pf_sdio_pio_block_count();
}

pf_sd_card_result_t pf_sd_card_read_blocks(uint32_t first_block, void *buffer,
                                           uint32_t block_count)
{
    if (!transport_selected) {
        return PF_SD_CARD_NOT_CONFIGURED;
    }
    if (active_transport == PF_SD_CARD_TRANSPORT_SPI) {
        return pf_sdspi_read_blocks(first_block, buffer, block_count);
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
    if (active_transport == PF_SD_CARD_TRANSPORT_SPI) {
        return pf_sdspi_write_blocks(first_block, buffer, block_count);
    }
    return translate_sdio_result(
        pf_sdio_pio_write_blocks(first_block, buffer, block_count));
}

pf_sd_card_result_t pf_sd_card_sync(void)
{
    if (!transport_selected) {
        return PF_SD_CARD_NOT_CONFIGURED;
    }
    if (active_transport == PF_SD_CARD_TRANSPORT_SPI) {
        return pf_sdspi_sync();
    }
    return translate_sdio_result(pf_sdio_pio_sync());
}