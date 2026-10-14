#ifndef PF_SD_CARD_H
#define PF_SD_CARD_H

#include "config/config.h"
#include "sd_card_block.h"
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SPI
#include "spi/sd_spi.h"
#else
#include "sdio_pio/sdio_pio.h"
#endif

typedef enum {
    PF_SD_CARD_TRANSPORT_SDIO_PIO = 0,
    PF_SD_CARD_TRANSPORT_SPI,
} pf_sd_card_transport_t;

typedef struct pf_sd_card_config {
    pf_sd_card_transport_t transport;
    union {
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SPI
        pf_sd_spi_config_t spi;
#else
        pf_sdio_pio_config_t sdio_pio;
#endif
    } bus;
} pf_sd_card_config_t;

pf_sd_card_result_t pf_sd_card_init(const pf_sd_card_config_t *config);
pf_sd_card_owner_t pf_sd_card_get_owner(void);
bool pf_sd_card_take_reinit_request(void);

#endif