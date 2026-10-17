#ifndef PF_STORAGE_SPI_H
#define PF_STORAGE_SPI_H

#include "../storage_transport.h"
#include "hardware/spi.h"

/** Caller-owned SPI device. Devices on one controller must use identical bus pins.
 * Calls are synchronous and externally serialized; contexts must initially be zero-initialized.
 * Wiring and instance must remain unchanged until deinit.
 */
typedef struct {
    spi_inst_t *instance; /**< SDK controller. */
    pf_storage_spi_pins_t pins; /**< Fixed physical signal allocation. */
    uint32_t clock_hz;    /**< Requested device clock, updated by set_clock. */
    uint32_t timeout_us;  /**< Per-byte FIFO deadline; zero uses SDK blocking bulk I/O. */
    bool initialized;    /**< This device holds a controller reference. */
} pf_storage_spi_t;

/** Validate controller pin routing and distinct chip select without touching hardware. */
bool pf_storage_spi_config_valid(const pf_storage_spi_t *context);
/** Acquire a mode-0 controller reference and configure idle-high CS. */
pf_storage_transport_result_t pf_storage_spi_init(pf_storage_spi_t *context);
/** Release this device; release bus pins only after the last reference. */
void pf_storage_spi_deinit(pf_storage_spi_t *context);
/** Set this device's clock and report the SDK-selected rate. */
pf_storage_transport_result_t pf_storage_spi_set_clock(pf_storage_spi_t *context, uint32_t requested_hz,
                                                uint32_t *actual_hz);
/** Assert/deassert CS; selection restores this device's clock. */
pf_storage_transport_result_t pf_storage_spi_select(pf_storage_spi_t *context, bool selected);
/** Exchange bytes; NULL transmit sends 0xff, NULL receive discards input.
 * CS, including error cleanup, is controlled by the caller; no bytes are replayed.
 */
pf_storage_transport_result_t pf_storage_spi_exchange(pf_storage_spi_t *context, const uint8_t *tx,
                                               uint8_t *rx, size_t length);
/** Execute one command/address/dummy/payload transaction with guaranteed CS cleanup. */
pf_storage_transport_result_t pf_storage_spi_transfer(pf_storage_spi_t *context,
    const pf_storage_serial_transaction_t *transaction);
/** Return a non-owning generic lifecycle/serial-dispatch handle. */
pf_storage_transport_t pf_storage_spi_bus(pf_storage_spi_t *context);

#endif
