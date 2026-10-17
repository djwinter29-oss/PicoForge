#ifndef PF_STORAGE_QSPI_PIO_H
#define PF_STORAGE_QSPI_PIO_H

#include "../storage_transport.h"
#include "hardware/pio.h"

/** Dedicated polling state machine, consecutive IO0..IO3, GPIO0..31 on both MCUs.
 * Context must initially be zero-initialized and outlive the bus handle.
 */
typedef struct {
    PIO pio;                /**< SDK PIO block. */
    pf_storage_qspi_pins_t pins; /**< Fixed physical signal allocation. */
    uint32_t clock_hz;       /**< Requested serial clock. */
    uint32_t sm;             /**< Claimed state machine. */
    uint32_t single_offset;  /**< Loaded single-lane program offset. */
    uint32_t quad_offset;    /**< Loaded quad-lane program offset. */
    bool initialized;       /**< Hardware resources are owned by this context. */
} pf_storage_qspi_pio_t;

/** Acquire state machine/program space and configure idle pins. */
pf_storage_transport_result_t pf_storage_qspi_pio_init(pf_storage_qspi_pio_t *context);
/** Release PIO resources and idle CS high. */
void pf_storage_qspi_pio_deinit(pf_storage_qspi_pio_t *context);
/** Set the serial clock using a representable PIO divider. */
pf_storage_transport_result_t pf_storage_qspi_pio_set_clock(pf_storage_qspi_pio_t *context,
                                                    uint32_t requested_hz, uint32_t *actual_hz);
/** Execute a single/quad payload transaction; always release CS after I/O. */
pf_storage_transport_result_t pf_storage_qspi_pio_transfer(pf_storage_qspi_pio_t *context,
    const pf_storage_serial_transaction_t *transaction);
/** Return a non-owning lifecycle and serial-dispatch handle. */
pf_storage_transport_t pf_storage_qspi_pio_transport(pf_storage_qspi_pio_t *context);

#endif
