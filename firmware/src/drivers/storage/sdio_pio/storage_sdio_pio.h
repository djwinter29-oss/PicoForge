#ifndef PF_STORAGE_SDIO_PIO_H
#define PF_STORAGE_SDIO_PIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hardware/pio.h"
#include "../storage_bus.h"

/** Physical clock generator currently owning CLK. */
typedef enum {
    PF_STORAGE_SDIO_CLOCK_UNOWNED,       /**< Clock stopped, lines released. */
    PF_STORAGE_SDIO_CLOCK_COMMAND,       /**< Command state machine drives CLK. */
    PF_STORAGE_SDIO_CLOCK_INITIALIZATION,/**< Standalone startup clocks. */
    PF_STORAGE_SDIO_CLOCK_DATA_TX,       /**< Standalone data clock. */
} pf_storage_sdio_clock_owner_t;

/** Physical failures; no card response or CRC interpretation is performed here. */
typedef enum {
    PF_STORAGE_SDIO_ERROR_NONE,        /**< No physical error. */
    PF_STORAGE_SDIO_ERROR_STATE,       /**< Invalid state or unavailable program memory. */
    PF_STORAGE_SDIO_ERROR_PIO_TIMEOUT, /**< PIO did not complete. */
    PF_STORAGE_SDIO_ERROR_DMA_TIMEOUT, /**< DMA did not complete. */
    PF_STORAGE_SDIO_ERROR_BUSY_TIMEOUT,/**< Data lines stayed busy. */
    PF_STORAGE_SDIO_ERROR_TX_STALL,    /**< Transmit FIFO starvation. */
    PF_STORAGE_SDIO_ERROR_RX_STALL,    /**< Receive FIFO overflow. */
} pf_storage_sdio_error_t;

/** Timeout domains for device-layer diagnostics. */
typedef enum {
    PF_STORAGE_SDIO_PHASE_COMMAND, /**< Command wire transfer. */
    PF_STORAGE_SDIO_PHASE_DATA,    /**< Data DMA and PIO. */
    PF_STORAGE_SDIO_PHASE_BUSY,    /**< DAT0 release. */
    PF_STORAGE_SDIO_PHASE_SETUP,   /**< Clock/program setup. */
} pf_storage_sdio_phase_t;

/** Optional bounded data-wait integration; must not clear the completion IRQ. */
typedef void (*pf_storage_sdio_wait_hook_t)(PIO pio, uint32_t completion_flag);
/** Optional device-layer diagnostic callback; context remains caller-owned. */
typedef void (*pf_storage_sdio_error_hook_t)(void *context, pf_storage_sdio_error_t error,
                                           pf_storage_sdio_phase_t phase);

/** Board-independent physical pin routing. */
typedef struct {
    PIO pio;                  /**< Reserved PIO instruction memory, pio0 or pio1. */
    pf_storage_sdio_pins_t pins; /**< Fixed physical signal allocation. */
} pf_storage_sdio_pio_config_t;

/** Caller-owned physical transport. Zero initialize before first use; never copy a live context. */
typedef struct {
    pf_storage_sdio_pio_config_t config; /**< Configuration retained for common-bus init. */
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
    uint32_t last_response_words[5];    /**< Last raw command response, including partial failures. */
    size_t last_response_word_count;   /**< Number of captured response words. */
    pf_storage_sdio_clock_owner_t clock_owner; /**< Current CLK owner. */
    pf_storage_sdio_error_t last_error; /**< Latest physical failure. */
    pf_storage_sdio_wait_hook_t wait_hook; /**< Optional bounded scheduler wait. */
    void (*idle_hook)(void);           /**< Optional bounded ordinary polling callback. */
    pf_storage_sdio_error_hook_t error_hook; /**< Optional diagnostic reporting. */
    void *hook_context;                /**< Borrowed diagnostic callback argument. */
    bool mode_restore_failed;          /**< Command program restoration failed. */
    bool command_program_loaded;       /**< Command program owns instruction memory. */
    bool data_rx_program_loaded;       /**< RX program owns instruction memory. */
    bool data_rx_fast;                 /**< RX program is cycle-locked. */
    bool clock_program_loaded;         /**< Clock program owns instruction memory. */
    bool data_tx_program_loaded;        /**< TX program owns instruction memory. */
    bool data_tx_fast;                 /**< TX program is cycle-locked. */
    bool initialization_prepared;      /**< Startup clocks completed. */
    bool dma_claimed;                  /**< DMA ownership is valid. */
    uint32_t clock_div_ticks;          /**< Command/clock divider in 1/256 units. */
} pf_storage_sdio_pio_t;

/** Return a non-owning common lifecycle descriptor; serial transfer is unsupported. */
pf_storage_bus_t pf_storage_sdio_pio_bus(pf_storage_sdio_pio_t *transport);
/** Acquire physical resources. Duplicate initialization leaves the live context unchanged. */
bool pf_storage_sdio_pio_init(pf_storage_sdio_pio_t *transport,
                              const pf_storage_sdio_pio_config_t *config);
/** Release all physical resources and zero the context. */
void pf_storage_sdio_pio_deinit(pf_storage_sdio_pio_t *transport);
/** Set a clock no faster than requested; does not change device protocol state. */
bool pf_storage_sdio_pio_set_clock(pf_storage_sdio_pio_t *transport, uint32_t clock_hz);
/** Reset hardware and emit exactly 80 released-line clocks at 400 kHz or less. */
bool pf_storage_sdio_pio_prepare_80_clocks(pf_storage_sdio_pio_t *transport);
/** Stop physical activity without issuing any device command. */
void pf_storage_sdio_pio_abort(pf_storage_sdio_pio_t *transport);
/** Restore command signaling after TX; does not terminate a device stream. */
bool pf_storage_sdio_pio_restore_command(pf_storage_sdio_pio_t *transport);
/** Transfer an already-framed 48-bit command; response words are raw PIO packing. */
bool pf_storage_sdio_pio_command(pf_storage_sdio_pio_t *transport, const uint8_t frame[6],
    uint16_t response_bits, uint32_t *response_words, size_t response_word_capacity);
/** Arm raw nibble RX before issuing the device command; buffers remain borrowed until cleanup. */
bool pf_storage_sdio_pio_rx_start(pf_storage_sdio_pio_t *transport, uint32_t *destination,
    uint32_t transfer_words, uint32_t nibble_count, uint32_t block_count);
/** Wait for raw RX with a bounded timeout. */
bool pf_storage_sdio_pio_rx_finish(pf_storage_sdio_pio_t *transport, uint32_t timeout_us);
/** Stop RX and clear its FIFO/IRQ without sending a command. */
void pf_storage_sdio_pio_rx_cleanup(pf_storage_sdio_pio_t *transport);
/** Poll raw RX completion, reporting stalls/timeouts; call within the timeout interval. */
bool pf_storage_sdio_pio_rx_poll(pf_storage_sdio_pio_t *transport, uint32_t start,
    uint32_t timeout_us, bool *complete);
/** Invoke the configured bounded RX scheduler hook without consuming the completion IRQ. */
void pf_storage_sdio_pio_rx_wait(pf_storage_sdio_pio_t *transport);
/** Return DMA-committed words with an acquire barrier, or false for an invalid DMA address. */
bool pf_storage_sdio_pio_rx_progress(pf_storage_sdio_pio_t *transport,
    const uint32_t *buffer, uint32_t capacity_words, uint32_t *completed_words);
/** Load physical TX programs, retaining them across multiple payloads. */
bool pf_storage_sdio_pio_tx_prepare(pf_storage_sdio_pio_t *transport);
/** Start a raw framed nibble payload; caller may prepare the next buffer while DMA runs. */
bool pf_storage_sdio_pio_tx_start(pf_storage_sdio_pio_t *transport,
    const uint32_t *image, uint32_t word_count, uint32_t nibble_count);
/** Wait for TX and DAT0 release; return the uninterpreted wire response token. */
bool pf_storage_sdio_pio_tx_finish(pf_storage_sdio_pio_t *transport,
    uint32_t timeout_us, uint32_t *response_word);
/** Bounded wait for DAT0 high without interpreting device state. */
bool pf_storage_sdio_pio_wait_ready(pf_storage_sdio_pio_t *transport, uint32_t timeout_us);

#endif
