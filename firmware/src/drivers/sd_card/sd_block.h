#ifndef PF_SD_BLOCK_H
#define PF_SD_BLOCK_H

#include <stdbool.h>
#include <stdint.h>

/** Reservation shared by both transports, telemetry and removal.
 * FreeRTOS: recursive, task-context only with a running scheduler.
 * Bare metal: no-op, single foreground context only; no ISR/second-core calls.
 * Pair every lock with unlock. Never hold across reply/USB waits.
 * Application storage access must use the owner-gated sd_media service. Raw driver calls do not enforce ownership.
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

/** Cumulative native counters, independent of optional performance profiling. */
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

typedef enum {
    /* Inclusive instrumentation windows, NOT exclusive latency phases.
     * DATA includes native TX response/busy and overlapped encode/decode;
     * STOP includes command/busy. RECOVERY is reserved/unmetered.
     * Never sum elapsed_us or compare phase values across transports directly.
     * See docs/sdcard-design.md, Profile Measurement Boundaries. ABI preserved. */
    PF_SD_PROFILE_COMMAND = 0,
    PF_SD_PROFILE_DATA,
    PF_SD_PROFILE_CRC,
    PF_SD_PROFILE_BUSY,
    PF_SD_PROFILE_STOP,
    PF_SD_PROFILE_RECOVERY, /* Reserved, unsupported on SPI and native; ABI slot retained. */
    PF_SD_PROFILE_PHASE_COUNT,
} pf_sd_profile_phase_t;

typedef enum {
    PF_SD_PROFILE_CRC_FAILURE = 0,
    PF_SD_PROFILE_BATCH_FALLBACK,
    PF_SD_PROFILE_TIMEOUT,
    PF_SD_PROFILE_RECOVERY_ATTEMPT, /* Reserved, unmetered on both transports. */
    PF_SD_PROFILE_EVENT_COUNT,
} pf_sd_profile_event_t;

/** Serialized-driver profiling; phase timings overlap and must not be summed. */
typedef struct {
    uint64_t elapsed_us[PF_SD_PROFILE_PHASE_COUNT];
    uint32_t calls[PF_SD_PROFILE_PHASE_COUNT];
    uint32_t events[PF_SD_PROFILE_EVENT_COUNT];
} pf_sd_profile_t;

/** Enable optional profiling, disabled at boot. This and the two profile calls below take the recursive facade
 * reservation internally, so they may be called with or without it held. */
void pf_sd_set_profiling_enabled(bool enabled);
/** Clear accumulated metrics without changing profiling enablement. */
void pf_sd_reset_profile(void);
/** Copy metrics to caller-owned storage; NULL is ignored. Requires serialized access. */
void pf_sd_get_profile(pf_sd_profile_t *profile);
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

/** Return the selected transport name (sdio, spi, or unconfigured).
 * The returned string has static lifetime and must not be modified.
 */
const char *pf_sd_transport_name(void);

/**
 * Set a callback run between polls of card waits that can last milliseconds
 * (write busy, command-line busy). Keep it short, such as taskYIELD(); NULL selects
 * plain busy polling. Set it before the first transfer; it is not synchronized.
 */
void pf_sd_set_idle_hook(void (*hook)(void));

#endif