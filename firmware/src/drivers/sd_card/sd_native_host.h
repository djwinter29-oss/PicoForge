#ifndef PF_SD_NATIVE_HOST_H
#define PF_SD_NATIVE_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sd_native.h"
#include "sd_native_protocol.h"
#include "../storage/sdio_pio/storage_sdio_pio.h"

/** Compatibility names for physical clock ownership, now owned by the transport. */
typedef pf_storage_sdio_clock_owner_t pf_sd_native_clock_owner_t;
/** Stopped physical clock. */
#define PF_SD_NATIVE_CLOCK_UNOWNED PF_STORAGE_SDIO_CLOCK_UNOWNED
/** Clock owned by command signaling. */
#define PF_SD_NATIVE_CLOCK_COMMAND PF_STORAGE_SDIO_CLOCK_COMMAND
/** Clock owned by released-line startup generation. */
#define PF_SD_NATIVE_CLOCK_INITIALIZATION PF_STORAGE_SDIO_CLOCK_INITIALIZATION
/** Clock owned by data transmission. */
#define PF_SD_NATIVE_CLOCK_DATA_TX PF_STORAGE_SDIO_CLOCK_DATA_TX

/** SD protocol state layered over a native wire transport. */
typedef struct {
    pf_storage_sdio_pio_t transport; /**< Owns GPIO, PIO, DMA and raw wire movement. */
    pf_sd_native_init_stage_t scr_stage; /**< Most recent SCR protocol stage. */
    uint32_t write_stream_next_sector; /**< Next LBA accepted by the open CMD25 stream. */
    uint8_t bus_width;                /**< Width negotiated with the card. */
    pf_sd_detail_error_t last_error; /**< Latest SD or mapped physical failure. */
    pf_sd_error_counters_t errors; /**< SD and mapped physical diagnostic counters. */
    bool write_stream_open;          /**< CMD25 requires CMD12 before other operations. */
    bool read_stream_unresolved;     /**< CMD18 may have reached the card and needs CMD12. */
    bool data_rx_multiblock_supported; /**< False after a qualified single-block fallback. */
} pf_sd_native_host_t;

/**
 * First use requires a zero-initialized host (static storage or {0}); this function
 * reads host->transport.pio before clearing the structure. Never pass an indeterminate host.
 * Duplicate init of a live host returns false without changing it or its resources.
 * Deinit releases resources and zeroes the host, allowing init again.
 */
bool pf_sd_native_host_init(pf_sd_native_host_t *host,
                              const pf_sd_native_config_t *config);
void pf_sd_native_host_deinit(pf_sd_native_host_t *host);
bool pf_sd_native_host_set_clock(pf_sd_native_host_t *host, uint32_t clock_hz);
/** Reset host hardware and generate 80 clocks at the configured low-speed rate. */
bool pf_sd_native_host_prepare_initialization(pf_sd_native_host_t *host);
/** Change the software bus-width state after the matching card command succeeds. */
bool pf_sd_native_host_set_bus_width(pf_sd_native_host_t *host, uint8_t width);
/** Abort host hardware without card commands; protocol stream state is retained. */
void pf_sd_native_host_abort(pf_sd_native_host_t *host);
bool pf_sd_native_host_command(pf_sd_native_host_t *host, uint8_t command,
                                 uint32_t argument, uint16_t response_bits,
                                 uint32_t *response_words, size_t response_word_capacity);
bool pf_sd_native_host_command_bytes(pf_sd_native_host_t *host, uint8_t command,
                                       uint32_t argument, uint16_t response_bits,
                                       uint8_t *response, size_t response_capacity);
bool pf_sd_native_host_read_sector(pf_sd_native_host_t *host, uint32_t sector,
                                     bool block_addressed, uint8_t block[512]);
/** Read and CRC-check the 8-byte SCR in the card's default one-bit mode. */
bool pf_sd_native_host_read_scr(pf_sd_native_host_t *host, uint32_t relative_card_address,
                               uint8_t scr[8]);
/** Read and CRC-check the 64-byte data block returned by SD CMD6. */
bool pf_sd_native_host_read_switch_status(pf_sd_native_host_t *host, uint32_t argument,
                                         uint8_t status[PF_SD_NATIVE_SWITCH_STATUS_SIZE]);
/** Read and CRC-check sectors; NULL blocks discards payload for qualification only. */
bool pf_sd_native_host_read_blocks(pf_sd_native_host_t *host, uint32_t first_sector,
                                      bool block_addressed, uint8_t *blocks,
                                      uint32_t block_count);
bool pf_sd_native_host_write_sector(pf_sd_native_host_t *host, uint32_t sector,
                                      bool block_addressed, const uint8_t block[512]);
/**
 * Write blocks with CMD25. The transfer stays open after success so a call starting at the next
 * sector continues it without a new CMD25; any other bus access must first call
 * pf_sd_native_host_recover(), which terminates the stream; earlier blocks may already be programmed.
 */
bool pf_sd_native_host_write_blocks(pf_sd_native_host_t *host, uint32_t first_sector,
                                      bool block_addressed, const uint8_t *blocks,
                                      uint32_t block_count);
/** @return true when a write stream is open and a write at @p sector would continue it. */
bool pf_sd_native_host_write_stream_continues(const pf_sd_native_host_t *host, uint32_t sector);
/** Resolve pending streams (CMD12) and failed command-mode restoration.
 * False leaves the host unusable or a stream unresolved; retry recovery before reinitializing.
 * Recovery never replays payload writes. */
bool pf_sd_native_host_recover(pf_sd_native_host_t *host);
/** Abandon an open stream after the storage owner confirms debounced removal; no card command is sent. */
void pf_sd_native_host_forget_removed_card(pf_sd_native_host_t *host);

#endif