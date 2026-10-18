#ifndef PF_SDIO_PIO_H
#define PF_SDIO_PIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hardware/pio.h"
#include "drivers/storage/sd_wire.h"

/** Physical clock ownership, private to this backend's resource management. */
typedef enum {
    PF_SDIO_PIO_CLOCK_UNOWNED, /**< Clock stopped and lines released. */
    PF_SDIO_PIO_CLOCK_COMMAND, /**< Command state machine owns CLK. */
    PF_SDIO_PIO_CLOCK_INITIALIZATION, /**< Standalone released-line clock generation. */
    PF_SDIO_PIO_CLOCK_DATA_TX, /**< Data transmission owns CLK. */
} pf_sdio_pio_clock_owner_t;

/** Optional bounded data-wait integration; must not clear the completion IRQ. */
typedef void (*pf_sdio_pio_wait_hook_t)(PIO pio, uint32_t completion_flag);

/** Board-independent physical pin routing. */
typedef struct {
    PIO pio;                  /**< Reserved PIO instruction memory, pio0 or pio1. */
    pf_storage_sdio_pins_t pins; /**< Fixed physical signal allocation. */
} pf_sdio_pio_config_t;

/** Caller-owned physical transport. Zero initialize before first use; never copy a live context. */
typedef struct {
    pf_sdio_pio_config_t config; /**< Configuration retained for common-bus init. */
    PIO pio;                           /**< NULL until resources are acquired. */
    uint command_sm;                   /**< Claimed command/clock state machine. */
    uint data_sm;                      /**< Claimed data state machine. */
    uint command_program_offset;       /**< Loaded command program origin. */
    uint data_rx_program_offset;       /**< Loaded receive program origin. */
    uint clock_program_offset;         /**< Loaded standalone clock origin. */
    uint data_tx_program_offset;       /**< Loaded transmit program origin. */
    uint dma_channel;                  /**< Claimed data DMA channel. */
    uint32_t system_clock_hz;          /**< Clock used to calculate dividers. */
    uint32_t current_clock_hz;         /**< Actual wire clock. */
    uint32_t previous_input_sync_bypass;/**< Saved GPIO synchronizer state. */
    uint32_t input_sync_bypass_mask;    /**< Bits owned by this transport. */
    pf_sdio_pio_clock_owner_t clock_owner; /**< Current CLK owner. */
    pf_storage_sd_wire_error_t last_error; /**< Latest physical failure. */
    pf_sdio_pio_wait_hook_t wait_hook; /**< Optional bounded scheduler wait. */
    void (*idle_hook)(void);           /**< Optional bounded ordinary polling callback. */
    pf_storage_sd_wire_error_hook_t error_hook; /**< Optional diagnostic reporting. */
    void *hook_context;                /**< Borrowed diagnostic callback argument. */
    bool mode_restore_failed;          /**< Command program restoration failed. */
    bool command_program_loaded;       /**< Command program owns instruction memory. */
    bool data_rx_program_loaded;       /**< RX program owns instruction memory. */
    bool data_rx_fast;                 /**< RX uses the shortened High-Speed loop (last request above 25 MHz). */
    bool clock_program_loaded;         /**< Clock program owns instruction memory. */
    bool data_tx_program_loaded;        /**< TX program owns instruction memory. */
    bool data_tx_fast;                 /**< TX is cycle-locked to CLK (last request above 25 MHz). */
    bool dma_claimed;                  /**< DMA ownership is valid. */
    uint32_t clock_div_ticks;          /**< Command/clock divider in 1/256 units. */
} pf_sdio_pio_t;

/** Return a non-owning common lifecycle descriptor; serial transfer is unsupported. */
pf_storage_bus_t pf_sdio_pio_bus(pf_sdio_pio_t *transport);
/** Enable the optional platform wait adapter for this backend. */
bool pf_sdio_pio_enable_wait(pf_sdio_pio_t *transport);
/** Acquire physical resources. Duplicate initialization leaves the live context unchanged. */
bool pf_sdio_pio_init(pf_sdio_pio_t *transport,
                      const pf_sdio_pio_config_t *config);
/** Release all physical resources and zero the context. */
void pf_sdio_pio_deinit(pf_sdio_pio_t *transport);
/** Set a clock no faster than requested; does not change device protocol state.
 * Requests above 25 MHz select High-Speed card timing and clamp to the fastest qualified cycle structure
 * (system clock / 4); a request that cannot be timed safely is rejected without changing the bus. */
bool pf_sdio_pio_set_clock(pf_sdio_pio_t *transport, uint32_t clock_hz);
/** Reset wire activity and emit caller-selected released-line clocks at the current rate. */
bool pf_sdio_pio_clocks(pf_sdio_pio_t *transport, uint32_t cycles, uint32_t timeout_us);
/** Stop physical activity without issuing any device command. */
void pf_sdio_pio_abort(pf_sdio_pio_t *transport);
/** Restore command signaling after TX; does not terminate a device stream. */
bool pf_sdio_pio_restore_command(pf_sdio_pio_t *transport);
/** Exchange six caller-framed CMD bytes; received includes partial RX bytes on failure. */
bool pf_sdio_pio_exchange(pf_sdio_pio_t *transport, const uint8_t *tx, size_t tx_size,
    uint16_t rx_bits, uint8_t *rx, size_t capacity, size_t *received, uint32_t timeout_us);
/** Arm raw nibble RX before issuing the device command; buffers remain borrowed until cleanup. */
bool pf_sdio_pio_rx_start(pf_sdio_pio_t *transport, uint32_t *destination,
    uint32_t transfer_words, uint32_t nibble_count, uint32_t block_count);
/** Wait for raw RX with a bounded timeout. */
bool pf_sdio_pio_rx_finish(pf_sdio_pio_t *transport, uint32_t timeout_us);
/** Stop RX and clear its FIFO/IRQ without sending a command. */
void pf_sdio_pio_rx_cleanup(pf_sdio_pio_t *transport);
/** Poll raw RX completion, reporting stalls/timeouts; call within the timeout interval. */
bool pf_sdio_pio_rx_poll(pf_sdio_pio_t *transport, uint32_t start,
    uint32_t timeout_us, bool *complete);
/** Invoke the configured bounded RX scheduler hook without consuming the completion IRQ. */
void pf_sdio_pio_rx_wait(pf_sdio_pio_t *transport);
/** Return DMA-committed words with an acquire barrier, or false for an invalid DMA address. */
bool pf_sdio_pio_rx_progress(pf_sdio_pio_t *transport,
    const uint32_t *buffer, uint32_t capacity_words, uint32_t *completed_words);
/** Load physical TX programs, retaining them across multiple payloads. */
bool pf_sdio_pio_tx_prepare(pf_sdio_pio_t *transport);
/** Start a raw framed nibble payload; caller may prepare the next buffer while DMA runs. */
bool pf_sdio_pio_tx_start(pf_sdio_pio_t *transport,
    const uint32_t *image, uint32_t word_count, uint32_t nibble_count);
/** Wait for TX and DAT0 release; return the uninterpreted wire response token. */
bool pf_sdio_pio_tx_finish(pf_sdio_pio_t *transport,
    uint32_t timeout_us, uint32_t *response_word);
/** Bounded wait for DAT0 high without interpreting device state. */
bool pf_sdio_pio_wait_ready(pf_sdio_pio_t *transport, uint32_t timeout_us);

#endif
