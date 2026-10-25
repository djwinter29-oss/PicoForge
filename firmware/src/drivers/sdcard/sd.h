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
/** Release one reservation level held by the current caller. */
void pf_sd_unlock(void);

/** Bytes per logical SD sector used by the public block API. */
#define PF_SD_BLOCK_SIZE 512u

/** Synchronous operation result; failure does not imply that no data was transferred. */
typedef enum {
    PF_SD_OK = 0, /**< Operation completed. */
    PF_SD_INVALID_ARGUMENT, /**< Configuration, buffer, range or transfer size is invalid. */
    PF_SD_CONFIG_LOCKED, /**< Requested configuration differs from the retained configuration. */
    PF_SD_NOT_CONFIGURED, /**< No valid backend initialization attempt has been made. */
    PF_SD_INIT_FAILED, /**< Physical setup or card identification failed. */
    PF_SD_NOT_READY, /**< Media is absent, uninitialized or no longer usable. */
    PF_SD_IO_ERROR, /**< Transfer or completion failed; partial effects are possible. */
} pf_sd_result_t;

/** Legacy implementation identifiers retained for source compatibility; not a runtime selector.
 * Current configuration uses pf_sd_link_mode_t, while CMake selects the implementation.
 * Numeric values are not interchangeable with pf_sd_link_mode_t.
 */
typedef enum {
    PF_SD_TRANSPORT_SDIO_PIO = 0, /**< Legacy native PIO implementation identifier. */
    PF_SD_TRANSPORT_SPI, /**< Legacy SPI implementation identifier. */
} pf_sd_transport_t;

/** Detailed native failure; public operation result values remain unchanged. */
typedef enum {
    PF_SD_DETAIL_NONE = 0, /**< No detailed failure recorded. */
    PF_SD_DETAIL_LEGACY_CARD, /**< Card version or capacity format is unsupported. */
    PF_SD_DETAIL_VOLTAGE, /**< Card voltage response is incompatible. */
    PF_SD_DETAIL_INIT_TIMEOUT, /**< Card power-up negotiation timed out. */
    PF_SD_DETAIL_R7, /**< CMD8 check pattern is invalid. */
    PF_SD_DETAIL_OCR_CCS, /**< OCR framing, voltage or capacity classification is invalid. */
    PF_SD_DETAIL_RESPONSE_CRC, /**< Response CRC or end bit is invalid. */
    PF_SD_DETAIL_RESPONSE_INDEX, /**< Response framing or command index is invalid. */
    PF_SD_DETAIL_CARD_STATUS, /**< Card reports an error or unexpected state. */
    PF_SD_DETAIL_FOUR_BIT, /**< Required four-bit capability or host width is unavailable. */
    PF_SD_DETAIL_HIGH_SPEED_UNSUPPORTED, /**< Card does not advertise usable High-Speed timing. */
    PF_SD_DETAIL_HIGH_SPEED_REJECTED, /**< Card did not complete the requested timing switch. */
    PF_SD_DETAIL_DATA_CRC, /**< Received data CRC failed or card rejected a write CRC. */
    PF_SD_DETAIL_DMA_TIMEOUT, /**< DMA completion deadline expired. */
    PF_SD_DETAIL_PIO_TIMEOUT, /**< PIO completion deadline expired. */
    PF_SD_DETAIL_BUSY_TIMEOUT, /**< Card data-line release deadline expired. */
    PF_SD_DETAIL_REMOVED, /**< Card absence was observed or confirmed. */
    PF_SD_DETAIL_HOST_STATE, /**< Host state, resource availability or clock request is invalid. */
    PF_SD_DETAIL_TX_STALL, /**< Physical transmit FIFO stalled. */
    PF_SD_DETAIL_RX_STALL, /**< Physical receive FIFO stalled. */
} pf_sd_detail_error_t;

/** Native counters accumulated since physical host initialization; card-init retries retain them.
 * Physical host deinitialization/reinitialization clears them. SPI reports zero.
 */
typedef struct {
    uint32_t command_crc_errors; /**< Response CRC failures. */
    uint32_t data_crc_errors; /**< Data CRC failures, including card CRC rejection tokens. */
    uint32_t command_timeouts; /**< Physical deadlines expired during CMD exchange. */
    uint32_t data_timeouts; /**< Physical deadlines expired during DAT transfer. */
    uint32_t dma_timeouts; /**< DMA-specific completion timeouts. */
    uint32_t busy_timeouts; /**< DAT0 release timeouts. */
    uint32_t pio_timeouts; /**< PIO-specific completion timeouts. */
    uint32_t tx_stalls; /**< Transmit FIFO stalls. */
    uint32_t rx_stalls; /**< Receive FIFO stalls. */
} pf_sd_error_counters_t;

/** Common media telemetry and negotiated timing, plus native-only diagnostics.
 * Native diagnostics can retain registers and capacity after failure; initialized alone gates I/O.
 * SPI leaves native-only fields zero and exposes identity/capacity only while initialized.
 */
typedef struct {
    bool initialized; /**< Current media is usable for block operations. */
    bool presence_known; /**< Presence comes from detection or successful initialization. */
    bool present; /**< Presence observation; meaningful only when presence_known is true. */
    bool cid_valid; /**< Retained CID is valid. */
    uint32_t bus_clock_hz; /**< Current transfer clock when initialized, otherwise zero. */
    uint32_t block_count; /**< Decoded capacity in 512-byte sectors; native may retain it after failure. */
    uint8_t cid[16]; /**< Card identification register; meaningful when cid_valid is true. */
    bool native_details_valid; /**< Native diagnostic fields are populated; false for SPI. */
    bool initialization_error; /**< Most recent native initialization attempt failed. */
    bool high_speed; /**< Negotiated High-Speed timing; SPI exposes this only while initialized. */
    uint8_t bus_width; /**< Native host's negotiated DAT width. */
    uint8_t sd_spec; /**< Decoded native SCR specification version. */
    uint8_t supported_bus_widths; /**< Native SCR bus-width capability bits. */
    uint16_t rca; /**< Native relative card address assigned by CMD3. */
    uint32_t ocr; /**< Native operating-conditions register. */
    uint8_t csd[16]; /**< Retained native card-specific data register. */
    uint8_t scr[8]; /**< Retained native SD configuration register. */
    uint32_t requested_clock_hz; /**< Native transfer target, including fallback selection. */
    uint32_t actual_clock_hz; /**< Current native physical clock, even when media is not ready. */
    uint32_t requested_init_clock_hz; /**< Native identification-clock target. */
    uint32_t actual_init_clock_hz; /**< Native identification clock observed before card negotiation. */
    uint32_t init_attempts; /**< Native initialization attempts since startup. */
    uint32_t high_speed_fallbacks; /**< Cumulative native fallbacks after High-Speed validation failed. */
    pf_sd_detail_error_t detail_error; /**< Latest native protocol or physical failure. */
    pf_sd_detail_error_t high_speed_error; /**< Native High-Speed negotiation/validation outcome. */
    pf_sd_error_counters_t errors; /**< Native physical-host-lifetime counters. */
} pf_sd_info_t;

/** @brief Protocol settings over a board-owned transport; its context outlives all SD access. */
typedef struct pf_sd_config {
    pf_sd_link_t transport; /**< Borrowed wire descriptor; mode must match the selected backend. */
    uint32_t clock_hz; /**< Requested target; rounded down. 0: 25 MHz; native: 25/50 MHz; SPI: 400 kHz-50 MHz. */
    bool native_default_speed_only; /**< Native only: disallow High-Speed negotiation even for a 50 MHz request. */
} pf_sd_config_t;

/** @brief Initialize or retry the build-selected backend.
 * Native SD locks a valid configuration before physical setup; SPI locks it after physical setup succeeds.
 * Once locked, configuration survives card-identification failures and removal; identical settings permit retry.
 */
pf_sd_result_t pf_sd_init(const pf_sd_config_t *config);

/** Return whether the selected card transport has initialized successfully. */
bool pf_sd_is_initialized(void);
/** Serialized storage owner notification after debounced card removal. */
void pf_sd_confirm_removed(void);

/** Return the initialized card's number of 512-byte sectors, or zero if unavailable. */
uint32_t pf_sd_block_count(void);

/** Read 512-byte sectors into a caller-owned buffer; failure may leave it partially updated.
 * Zero count is a no-op once the backend's configuration/readiness checks pass.
 */
pf_sd_result_t pf_sd_read_blocks(uint32_t first_block, void *buffer,
                                uint32_t block_count);

/** Write 512-byte sectors from a caller-owned buffer; failure may leave some sectors committed.
 * Uncertain writes are never automatically replayed. Zero count follows the same checks as reads.
 */
pf_sd_result_t pf_sd_write_blocks(uint32_t first_block, const void *buffer,
                                 uint32_t block_count);

/** Complete pending protocol writes; cannot undo sectors already committed by a failed operation. */
pf_sd_result_t pf_sd_sync(void);

/** Snapshot current/retained telemetry; presence is unknown until detect or init succeeds.
 * NULL output is ignored. Retained diagnostics do not establish media readiness.
 */
void pf_sd_get_info(pf_sd_info_t *info);

/**
 * Set a callback run between polls of card waits that can last milliseconds
 * (write busy, command-line busy). Keep it short, such as taskYIELD(); NULL selects
 * plain busy polling. Set it before the first transfer; it is not synchronized.
 */
void pf_sd_set_idle_hook(void (*hook)(void));

#endif