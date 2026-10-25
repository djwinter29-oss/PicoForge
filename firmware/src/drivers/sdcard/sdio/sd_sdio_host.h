/** @file
 * @brief Serialized native SD command/data sequencing and stream recovery.
 *
 * Non-reentrant: all host instances share one receive scratch buffer. Callers must hold the SD reservation
 * across operations, or provide equivalent single-context serialization. No operation acquires that reservation.
 */
#ifndef PF_SD_SDIO_HOST_H
#define PF_SD_SDIO_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sd_sdio_internal.h"
#include "sd_protocol.h"
#include "drivers/sdcard/sdio/sd_link_sdio.h"

/** SD protocol state layered over a native wire transport. */
typedef struct {
    pf_sd_link_t transport; /**< Borrows board/storage-owned wire state. */
    pf_sd_sdio_init_stage_t scr_stage; /**< Most recent SCR protocol stage. */
    uint32_t write_stream_next_sector; /**< Next LBA accepted by the open CMD25 stream. */
    uint8_t bus_width;                /**< Width negotiated with the card. */
    pf_sd_detail_error_t last_error; /**< Latest SD or mapped physical failure. */
    pf_sd_error_counters_t errors; /**< SD and mapped physical diagnostic counters. */
    uint32_t last_response_words[5]; /**< Legacy packed response diagnostic, including partial capture. */
    size_t last_response_word_count; /**< Number of captured response words. */
    bool write_stream_open;          /**< CMD25 requires CMD12 before other operations. */
    bool read_stream_unresolved;     /**< CMD18 may have reached the card and needs CMD12. */
    bool data_rx_multiblock_supported; /**< False after a qualified single-block fallback. */
} pf_sd_sdio_host_t;

/** @brief Decode a short response and record framing/CRC failures in the protocol state. */
static inline bool pf_sd_sdio_host_parse_response(pf_sd_sdio_host_t *host, uint8_t command,
                                                    const uint8_t response[6], uint32_t *argument)
{
    pf_sd_detail_error_t error = pf_sd_native_decode_response(command, response, argument, true);
    if (error == PF_SD_DETAIL_NONE) {
        return true;
    }
    if (error == PF_SD_DETAIL_RESPONSE_CRC) {
        ++host->errors.command_crc_errors;
    }
    host->last_error = error;
    return false;
}

/** @brief Validate status using the caller's R1 or R6 mask; R7 is classified separately. */
static inline bool pf_sd_sdio_host_response_status_ok(pf_sd_sdio_host_t *host, uint32_t argument, uint32_t mask)
{
    if ((argument & mask) == 0u) {
        return true;
    }
    host->last_error = PF_SD_DETAIL_CARD_STATUS;
    return false;
}

/**
 * First use requires a zero-initialized host (static storage or {0}); this function
 * checks transport readiness before clearing the structure. Never pass an indeterminate host.
 * Duplicate init of a live host returns false without changing it or its resources.
 * Deinit releases resources and zeroes the host, allowing init again.
 * The host must remain at the same address until deinit; diagnostic callbacks borrow it.
 */
bool pf_sd_sdio_host_init(pf_sd_sdio_host_t *host, const pf_sd_config_t *config);
void pf_sd_sdio_host_deinit(pf_sd_sdio_host_t *host);
bool pf_sd_sdio_host_set_clock(pf_sd_sdio_host_t *host, uint32_t clock_hz);
/** Reset wire activity and generate 80 released-line clocks at 400 kHz or less. */
bool pf_sd_sdio_host_prepare_initialization(pf_sd_sdio_host_t *host);
/** Change the software bus-width state after the matching card command succeeds. */
bool pf_sd_sdio_host_set_bus_width(pf_sd_sdio_host_t *host, uint8_t width);
/** Abort host hardware without card commands; protocol stream state is retained. */
void pf_sd_sdio_host_abort(pf_sd_sdio_host_t *host);
/** Build one SD command and exchange raw response bytes; the host retains partial diagnostics. */
bool pf_sd_sdio_host_command_bytes(pf_sd_sdio_host_t *host, uint8_t command,
                                       uint32_t argument, uint16_t response_bits,
                                       uint8_t *response, size_t response_capacity);
/** @brief Read and CRC-check one SDHC/SDXC sector; NULL discards the payload. */
bool pf_sd_sdio_host_read_sector(pf_sd_sdio_host_t *host, uint32_t sector, uint8_t block[512]);
/** Read and CRC-check the 8-byte SCR in the card's default one-bit mode. */
bool pf_sd_sdio_host_read_scr(pf_sd_sdio_host_t *host, uint32_t relative_card_address,
                               uint8_t scr[8]);
/** Read and CRC-check the 64-byte data block returned by SD CMD6. */
bool pf_sd_sdio_host_read_switch_status(pf_sd_sdio_host_t *host, uint32_t argument,
                                         uint8_t status[PF_SD_NATIVE_SWITCH_STATUS_SIZE]);
/** Read and CRC-check SDHC/SDXC sector addresses; NULL blocks discards payload for qualification only. */
bool pf_sd_sdio_host_read_blocks(pf_sd_sdio_host_t *host, uint32_t first_sector,
                                  uint8_t *blocks, uint32_t block_count);
/** @brief Write one SDHC/SDXC sector with CMD24; uncertain completion is never replayed. */
bool pf_sd_sdio_host_write_sector(pf_sd_sdio_host_t *host, uint32_t sector,
                                   const uint8_t block[512]);
/**
 * Write blocks with CMD25. The transfer stays open after success so a call starting at the next
 * sector continues it without a new CMD25; any other bus access must first call
 * pf_sd_sdio_host_recover(), which terminates the stream; earlier blocks may already be programmed.
 */
bool pf_sd_sdio_host_write_blocks(pf_sd_sdio_host_t *host, uint32_t first_sector,
                                   const uint8_t *blocks, uint32_t block_count);
/** @return true when a write stream is open and a write at @p sector would continue it. */
bool pf_sd_sdio_host_write_stream_continues(const pf_sd_sdio_host_t *host, uint32_t sector);
/** Resolve pending streams (CMD12) and failed command-mode restoration.
 * False leaves the host unusable or a stream unresolved; retry recovery before reinitializing.
 * Recovery never replays payload writes. */
bool pf_sd_sdio_host_recover(pf_sd_sdio_host_t *host);
/** Abandon an open stream after the storage owner confirms debounced removal; no card command is sent. */
void pf_sd_sdio_host_forget_removed_card(pf_sd_sdio_host_t *host);

#endif