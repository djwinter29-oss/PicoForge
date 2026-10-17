#ifndef PF_NOR_QSPI_PIO_H
#define PF_NOR_QSPI_PIO_H

#include "../nor_flash_transport.h"
#include "hardware/pio.h"
#include "../../storage/qspi_pio/storage_qspi_pio.h"

/** Dedicated PIO state machine with consecutive IO0..IO3; initially zero-initialize.
 * Uses polling, not DMA. Pin range is restricted to GPIO0..31 on both MCUs.
 */
typedef pf_storage_qspi_pio_t pf_nor_qspi_pio_t;

/** Claim a state machine/program memory; caller owns context until deinit. */
pf_nor_result_t pf_nor_qspi_pio_init(pf_nor_qspi_pio_t *context, pf_nor_transport_t *transport);
/** Release PIO resources, idle CS high and release data/clock pins. */
void pf_nor_qspi_pio_deinit(pf_nor_qspi_pio_t *context);

#endif
