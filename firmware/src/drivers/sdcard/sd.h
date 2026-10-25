/** @file
 * @brief Public synchronous SD-card API: configuration, reservations, block I/O and diagnostics.
 */
#ifndef PF_SD_H
#define PF_SD_H

#include <stdbool.h>
#include <stdint.h>

#include "sd_link.h"

/** Reservation shared by both transports, telemetry and removal.
 * FreeRTOS: recursive, task-context only with a running scheduler.
 * Bare metal: no-op, single foreground context only; no ISR/second-core calls.
 * Pair every lock with unlock. Never hold across reply/USB waits.
 * Application storage access must use the owner-gated storage service (middleware/storage/storage.h).
 * Raw driver calls do not enforce ownership.
 */
void pf_sd_lock(void);
/** Attempt a reservation without waiting; unlock only on success.
 * Always succeeds under the bare-metal port's single-context contract.
 */
bool pf_sd_try_lock(void);
void pf_sd_unlock(void);

#define PF_SD_BLOCK_SIZE 512u

typedef enum {
    PF_SD_OK = 0,
    PF_SD_INVALID_ARGUMENT,
    PF_SD_CONFIG_LOCKED,
    PF_SD_NOT_CONFIGURED,
    PF_SD_INIT_FAILED,
    PF_SD_NOT_READY,
    PF_SD_IO_ERROR,
} pf_sd_result_t;

typedef enum {
    PF_SD_TRANSPORT_SDIO_PIO = 0,
    PF_SD_TRANSPORT_SPI,
} pf_sd_transport_t;

/** Detailed native failure; public operation result values remain unchanged. */
typedef enum {
    PF_SD_DETAIL_NONE = 0,
    PF_SD_DETAIL_LEGACY_CARD,
    PF_SD_DETAIL_VOLTAGE,
    PF_SD_DETAIL_INIT_TIMEOUT,
    PF_SD_DETAIL_R7,
    PF_SD_DETAIL_OCR_CCS,
    PF_SD_DETAIL_RESPONSE_CRC,
    PF_SD_DETAIL_RESPONSE_INDEX,
    PF_SD_DETAIL_CARD_STATUS,
    PF_SD_DETAIL_FOUR_BIT,
    PF_SD_DETAIL_HIGH_SPEED_UNSUPPORTED,
    PF_SD_DETAIL_HIGH_SPEED_REJECTED,
    PF_SD_DETAIL_DATA_CRC,
    PF_SD_DETAIL_DMA_TIMEOUT,
    PF_SD_DETAIL_PIO_TIMEOUT,
    PF_SD_DETAIL_BUSY_TIMEOUT,
    PF_SD_DETAIL_REMOVED,
    PF_SD_DETAIL_HOST_STATE,
    PF_SD_DETAIL_TX_STALL,
    PF_SD_DETAIL_RX_STALL,
} pf_sd_detail_error_t;

/** Cumulative native counters. */
typedef struct {
    uint32_t command_crc_errors;
    uint32_t data_crc_errors;
    uint32_t command_timeouts;
    uint32_t data_timeouts;
    uint32_t dma_timeouts;
    uint32_t busy_timeouts;
    uint32_t pio_timeouts;
    uint32_t tx_stalls;
    uint32_t rx_stalls;
} pf_sd_error_counters_t;

typedef struct {
    bool initialized;
    bool presence_known;
    bool present;
    bool cid_valid;
    uint32_t bus_clock_hz;
    uint32_t block_count;
    uint8_t cid[16];
    /** Native-only diagnostic snapshot. Zero for other transports. Raw registers
     * and capacity remain available after a failure; initialized gates I/O. */
    bool native_details_valid;
    bool initialization_error;
    bool high_speed;
    uint8_t bus_width;
    uint8_t sd_spec;
    uint8_t supported_bus_widths;
    uint16_t rca;
    uint32_t ocr;
    uint8_t csd[16];
    uint8_t scr[8];
    uint32_t requested_clock_hz;
    uint32_t actual_clock_hz;
    uint32_t requested_init_clock_hz;
    uint32_t actual_init_clock_hz;
    uint32_t init_attempts;
    uint32_t high_speed_fallbacks;
    pf_sd_detail_error_t detail_error;
    pf_sd_detail_error_t high_speed_error;
    pf_sd_error_counters_t errors;
} pf_sd_info_t;

/** @brief Protocol settings over a board-owned transport; its context outlives all SD access. */
typedef struct pf_sd_config {
    pf_sd_link_t transport; /**< Borrowed wire descriptor; mode must match the selected backend. */
    uint32_t clock_hz; /**< Requested target; rounded down. 0: 25 MHz; native: 25/50 MHz; SPI: 400 kHz-50 MHz. */
    bool native_default_speed_only; /**< Native only: disallow High-Speed negotiation even for a 50 MHz request. */
} pf_sd_config_t;

/** @brief Initialize the build-selected backend; the first accepted configuration remains locked. */
pf_sd_result_t pf_sd_init(const pf_sd_config_t *config);

/** Return whether the selected card transport has initialized successfully. */
bool pf_sd_is_initialized(void);
/** Serialized storage owner notification after debounced card removal. */
void pf_sd_confirm_removed(void);

/** Return the initialized card's number of 512-byte sectors, or zero if unavailable. */
uint32_t pf_sd_block_count(void);

/** Read 512-byte sectors into a caller-owned buffer. */
pf_sd_result_t pf_sd_read_blocks(uint32_t first_block, void *buffer,
                                uint32_t block_count);

/** Write 512-byte sectors from a caller-owned buffer. */
pf_sd_result_t pf_sd_write_blocks(uint32_t first_block, const void *buffer,
                                 uint32_t block_count);

/** Complete any pending write. */
pf_sd_result_t pf_sd_sync(void);

/** Snapshot initialized card telemetry; presence is unknown until detect or init succeeds. */
void pf_sd_get_info(pf_sd_info_t *info);

/**
 * Set a callback run between polls of card waits that can last milliseconds
 * (write busy, command-line busy). Keep it short, such as taskYIELD(); NULL selects
 * plain busy polling. Set it before the first transfer; it is not synchronized.
 */
void pf_sd_set_idle_hook(void (*hook)(void));

#endif