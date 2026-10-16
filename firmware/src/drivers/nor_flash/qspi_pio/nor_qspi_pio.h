#ifndef PF_NOR_QSPI_PIO_H
#define PF_NOR_QSPI_PIO_H

#include "../nor_flash_transport.h"
#include "hardware/pio.h"

/** Dedicated PIO state machine with consecutive IO0..IO3; initially zero-initialize.
 * Uses polling, not DMA. Pin range is restricted to GPIO0..31 on both MCUs.
 */
typedef struct {
    PIO pio;
    uint32_t clock_gpio;
    uint32_t data_gpio;
    uint32_t cs_gpio;
    uint32_t clock_hz;
    uint32_t sm;
    uint32_t single_offset;
    uint32_t quad_offset;
    bool initialized;
} pf_nor_qspi_pio_t;

/** Claim a state machine/program memory; caller owns context until deinit. */
pf_nor_result_t pf_nor_qspi_pio_init(pf_nor_qspi_pio_t *context, pf_nor_transport_t *transport);
/** Release PIO resources, idle CS high and release data/clock pins. */
void pf_nor_qspi_pio_deinit(pf_nor_qspi_pio_t *context);

#endif
