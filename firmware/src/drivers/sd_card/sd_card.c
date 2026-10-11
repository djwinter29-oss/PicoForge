#include "sd_card.h"

#include "sdio_pio/sdio_pio.h"
#include "spi/sd_spi.h"

static bool transport_selected;
static pf_sd_card_transport_t active_transport;

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
        result = pf_sd_spi_init(&config->bus.spi);
    } else if (config->transport == PF_SD_CARD_TRANSPORT_SDIO_PIO) {
        if (transport_selected && (active_transport != config->transport)) {
            return PF_SD_CARD_CONFIG_LOCKED;
        }
        result = pf_sdio_pio_init(&config->bus.sdio_pio);
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
        return pf_sd_spi_is_initialized();
    }
    return pf_sdio_pio_is_initialized();
}

uint32_t pf_sd_card_block_count(void)
{
    if (!pf_sd_card_is_initialized()) {
        return 0u;
    }
    return (active_transport == PF_SD_CARD_TRANSPORT_SPI)
        ? pf_sd_spi_block_count()
        : pf_sdio_pio_block_count();
}

pf_sd_card_result_t pf_sd_card_read_blocks(uint32_t first_block, void *buffer,
                                           uint32_t block_count)
{
    if (!transport_selected) {
        return PF_SD_CARD_NOT_CONFIGURED;
    }
    if (active_transport == PF_SD_CARD_TRANSPORT_SPI) {
        return pf_sd_spi_read_blocks(first_block, buffer, block_count);
    }
    return pf_sdio_pio_read_blocks(first_block, buffer, block_count);
}

pf_sd_card_result_t pf_sd_card_write_blocks(uint32_t first_block,
                                            const void *buffer,
                                            uint32_t block_count)
{
    if (!transport_selected) {
        return PF_SD_CARD_NOT_CONFIGURED;
    }
    if (active_transport == PF_SD_CARD_TRANSPORT_SPI) {
        return pf_sd_spi_write_blocks(first_block, buffer, block_count);
    }
    return pf_sdio_pio_write_blocks(first_block, buffer, block_count);
}

pf_sd_card_result_t pf_sd_card_sync(void)
{
    if (!transport_selected) {
        return PF_SD_CARD_NOT_CONFIGURED;
    }
    if (active_transport == PF_SD_CARD_TRANSPORT_SPI) {
        return pf_sd_spi_sync();
    }
    return pf_sdio_pio_sync();
}