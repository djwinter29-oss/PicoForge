/** @file
 * @brief Native CMD/DAT operations implemented by SDIO-PIO; no card-command or stream policy.
 *
 * Calls are synchronous and require external serialization with all access to the same physical context.
 * Descriptors and callback contexts remain borrowed until deinitialization; callbacks must not reenter the driver.
 */
#ifndef PF_SD_LINK_SDIO_H
#define PF_SD_LINK_SDIO_H

#include "sd_link.h"

/** Physical failure, never a decoded device response or CRC result. */
typedef enum {
    PF_SD_LINK_SDIO_ERROR_NONE,         /**< No failure. */
    PF_SD_LINK_SDIO_ERROR_STATE,        /**< Invalid or unavailable wire state. */
    PF_SD_LINK_SDIO_ERROR_PIO_TIMEOUT,  /**< Wire engine deadline expired. */
    PF_SD_LINK_SDIO_ERROR_DMA_TIMEOUT,  /**< Memory transfer deadline expired. */
    PF_SD_LINK_SDIO_ERROR_BUSY_TIMEOUT, /**< Requested line level was not reached. */
    PF_SD_LINK_SDIO_ERROR_TX_STALL,     /**< Transmit starvation. */
    PF_SD_LINK_SDIO_ERROR_RX_STALL,     /**< Receive overflow. */
} pf_sd_link_sdio_error_t;

/** Physical failure domain for upper-layer diagnostics. */
typedef enum {
    PF_SD_LINK_SDIO_PHASE_COMMAND, /**< CMD exchange. */
    PF_SD_LINK_SDIO_PHASE_DATA,    /**< DAT transfer. */
    PF_SD_LINK_SDIO_PHASE_BUSY,    /**< Line-level wait. */
    PF_SD_LINK_SDIO_PHASE_SETUP,   /**< Resource/clock setup. */
} pf_sd_link_sdio_phase_t;

/** Synchronous borrowed failure callback. */
typedef void (*pf_sd_link_sdio_error_hook_t)(void *context, pf_sd_link_sdio_error_t error,
                                            pf_sd_link_sdio_phase_t phase);

/** Physical availability and line/clock observation; no PIO/DMA ownership or response packing. */
typedef struct {
    bool ready; /**< Resources acquired. */
    bool healthy; /**< Wire mode is usable or can return to idle. */
    bool data_high; /**< Current DAT0 level, not interpreted device readiness. */
    uint32_t current_clock_hz; /**< Calculated actual wire clock. */
    uint32_t maximum_clock_hz; /**< Fastest clock the backend will time safely; faster requests clamp or fail. */
} pf_sd_link_sdio_state_t;

/** @name Setup and observation
 * @{
 */
/** Validate the native descriptor without acquiring resources. */
bool pf_sd_link_sdio_valid(const pf_sd_link_t *bus);
/** Observe physical state; unavailable descriptors report ready=false, healthy=false. */
pf_sd_link_sdio_state_t pf_sd_link_sdio_state(const pf_sd_link_t *bus);
/** Borrow callbacks until deinit; the callback context must not move. */
bool pf_sd_link_sdio_hooks(const pf_sd_link_t *bus,
                           pf_sd_link_sdio_error_hook_t error, void *context, void (*idle)(void));
/** @} */

/** @name CMD signaling
 * @{
 */
/** Generate caller-selected clock count at the current rate, with CMD/DAT released. */
bool pf_sd_link_sdio_clocks(const pf_sd_link_t *bus, uint32_t cycles, uint32_t timeout_us);
/** Finish physical turnaround to CMD idle; never finish/replay a device write. */
bool pf_sd_link_sdio_idle(const pf_sd_link_t *bus);
/** Exchange caller-framed CMD bytes; received includes partial bytes on a wire failure. */
bool pf_sd_link_sdio_exchange(const pf_sd_link_t *bus, const uint8_t *tx, size_t tx_size,
                             uint16_t rx_bits, uint8_t *rx, size_t capacity, size_t *received,
                             uint32_t timeout_us);
/** @} */

/** @name DAT reception
 * Arm before CMD, then poll/progress/wait or finish. Always cancel to release RX state, even after success.
 * @{
 */
/** Arm framed DAT reception before CMD; words/nibbles/blocks describe raw wire buffers. */
bool pf_sd_link_sdio_rx_start(const pf_sd_link_t *bus, uint32_t *buffer,
                             uint32_t words, uint32_t nibbles, uint32_t blocks);
/** Poll RX completion and caller-selected deadline. */
bool pf_sd_link_sdio_rx_poll(const pf_sd_link_t *bus, uint32_t start,
                            uint32_t timeout_us, bool *complete);
/** Report DMA-committed words with an acquire barrier. */
bool pf_sd_link_sdio_rx_progress(const pf_sd_link_t *bus,
                                const uint32_t *buffer, uint32_t capacity, uint32_t *words);
/** Perform one bounded scheduler wait without consuming completion. */
void pf_sd_link_sdio_rx_wait(const pf_sd_link_t *bus);
/** Bounded RX completion wait; cancel remains required after completion or failure. */
bool pf_sd_link_sdio_rx_finish(const pf_sd_link_t *bus, uint32_t timeout_us);
/** Stop RX DMA and clear FIFO/completion state. */
void pf_sd_link_sdio_rx_cancel(const pf_sd_link_t *bus);
/** @} */

/** @name DAT transmission and busy signaling
 * @{
 */
/** Start raw DAT TX, retaining physical resources across adjacent payloads. */
bool pf_sd_link_sdio_tx_start(const pf_sd_link_t *bus,
                             const uint32_t *buffer, uint32_t words, uint32_t nibbles);
/** Bounded TX completion, returning raw response bits without interpreting acceptance. */
bool pf_sd_link_sdio_tx_finish(const pf_sd_link_t *bus, uint32_t timeout_us, uint32_t *response);
/** Bounded wait for DAT0 high; caller supplies device-policy deadline. */
bool pf_sd_link_sdio_wait_high(const pf_sd_link_t *bus, uint32_t timeout_us);
/** @} */

#endif
