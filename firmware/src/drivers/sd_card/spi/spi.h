#ifndef PF_SPI_H
#define PF_SPI_H

#include "drivers/sd_card/link/sd_link_serial.h"
#include "drivers/sd_card/spi/spi_pins.h"
#include "hardware/spi.h"

/** Caller-owned SPI device. Devices on one controller must use identical bus pins.
 * Calls are synchronous and externally serialized; contexts must initially be zero-initialized.
 * Wiring and instance must remain unchanged until deinit.
 */
typedef struct {
    spi_inst_t *instance; /**< SDK controller. */
    pf_spi_pins_t pins;   /**< Fixed physical signal allocation. */
    uint32_t clock_hz;    /**< Requested device clock, updated by set_clock. */
    uint32_t timeout_us;  /**< Maximum stalled FIFO interval; zero uses SDK blocking bulk I/O. */
    bool initialized;    /**< This device holds a controller reference. */
} pf_spi_t;

/** Check controller pin roles; callers separately validate bounds and conflicts. */
bool pf_spi_pins_match_controller(uint32_t spi_index, uint32_t clock_gpio,
                                  uint32_t mosi_gpio, uint32_t miso_gpio);
/** Validate controller pin routing and distinct chip select without touching hardware. */
bool pf_spi_config_valid(const pf_spi_t *context);
/** Acquire a mode-0 controller reference and configure idle-high CS. */
pf_sd_link_result_t pf_spi_init(pf_spi_t *context);
/** Release this device; release bus pins only after the last reference. */
void pf_spi_deinit(pf_spi_t *context);
/** Set this device's clock and report the SDK-selected rate. */
pf_sd_link_result_t pf_spi_set_clock(pf_spi_t *context, uint32_t requested_hz,
                                              uint32_t *actual_hz);
/** Assert/deassert CS; selection restores this device's clock. */
pf_sd_link_result_t pf_spi_select(pf_spi_t *context, bool selected);
/** Exchange bytes; NULL transmit sends 0xff, NULL receive discards input.
 * CS, including error cleanup, is controlled by the caller; no bytes are replayed.
 */
pf_sd_link_result_t pf_spi_exchange(pf_spi_t *context, const uint8_t *tx,
                                             uint8_t *rx, size_t length);
/** Execute one command/address/dummy/payload transaction with guaranteed CS cleanup. */
pf_sd_link_result_t pf_spi_transfer(pf_spi_t *context,
    const pf_sd_link_serial_phase_t *phases, size_t count);
/** Return a non-owning generic lifecycle/serial-dispatch handle. */
pf_sd_link_t pf_spi_bus(pf_spi_t *context);

#endif
