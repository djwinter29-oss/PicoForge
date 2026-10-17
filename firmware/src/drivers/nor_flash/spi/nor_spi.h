#ifndef PF_NOR_SPI_H
#define PF_NOR_SPI_H

#include "../nor_flash_transport.h"
#include "hardware/spi.h"
#include "../../storage/spi/storage_spi.h"

/** Compatibility configuration; physical signaling is owned by storage_spi. */
typedef struct {
    spi_inst_t *instance; /**< SDK controller, also usable by other serialized SPI devices. */
    uint32_t clock_gpio;  /**< Shared clock output. */
    uint32_t mosi_gpio;   /**< Shared controller transmit pin. */
    uint32_t miso_gpio;   /**< Shared controller receive pin. */
    uint32_t wp_gpio;     /**< NOR write-protect, driven inactive high. */
    uint32_t hold_gpio;   /**< NOR hold, driven inactive high. */
    uint32_t cs_gpio;     /**< NOR-specific active-low chip select. */
    uint32_t clock_hz;    /**< Requested NOR clock, at most 10 MHz. */
    bool initialized;    /**< Compatibility adapter owns its physical bus reference. */
    pf_storage_spi_t bus; /**< Shared physical device context, owned by this adapter. */
} pf_nor_spi_t;

/** Initialize SPI mode 0 and idle-high CS/WP/HOLD. Context must outlive transport. */
pf_nor_result_t pf_nor_spi_init(pf_nor_spi_t *context, pf_nor_transport_t *transport);
/** Release pins/controller after all operations finish; does not free context. */
void pf_nor_spi_deinit(pf_nor_spi_t *context);

#endif
