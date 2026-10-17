#ifndef PF_SD_H
#define PF_SD_H

#include "config/config.h"
#include "sd_block.h"
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SPI
#include "sd_spi.h"
#else
#include "sd_native.h"
#endif

typedef struct pf_sd_config {
    pf_sd_transport_t transport;
    union {
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SPI
        pf_sd_spi_config_t spi;
#else
        pf_sd_native_config_t native;
#endif
    } bus;
} pf_sd_config_t;

pf_sd_result_t pf_sd_init(const pf_sd_config_t *config);
pf_sd_owner_t pf_sd_get_owner(void);
bool pf_sd_take_reinit_request(void);

#endif