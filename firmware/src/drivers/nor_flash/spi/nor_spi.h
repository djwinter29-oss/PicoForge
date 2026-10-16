#ifndef PF_NOR_SPI_H
#define PF_NOR_SPI_H

#include "../nor_flash_transport.h"
#include "hardware/spi.h"

/** Caller-owned dedicated SPI controller; do not share with another driver. */
typedef struct {
    spi_inst_t *instance;
    uint32_t clock_gpio;
    uint32_t mosi_gpio;
    uint32_t miso_gpio;
    uint32_t wp_gpio;
    uint32_t hold_gpio;
    uint32_t cs_gpio;
    uint32_t clock_hz;
    bool initialized;
} pf_nor_spi_t;

/** Initialize SPI mode 0 and idle-high CS/WP/HOLD. Context must outlive transport. */
pf_nor_result_t pf_nor_spi_init(pf_nor_spi_t *context, pf_nor_transport_t *transport);
/** Release pins/controller after all operations finish; does not free context. */
void pf_nor_spi_deinit(pf_nor_spi_t *context);

#endif
