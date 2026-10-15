#ifndef PF_SD_CARD_BLOCK_H
#define PF_SD_CARD_BLOCK_H

#include <stdbool.h>
#include <stdint.h>

/** Reservation shared by both transports, telemetry and removal.
 * FreeRTOS: recursive, task-context only with a running scheduler.
 * Bare metal: no-op, single foreground context only; no ISR/second-core calls.
 * Pair every lock with unlock. Never hold across reply/USB waits.
 * Backends must be accessed through the facade.
 */
void pf_sd_card_lock(void);
/** Attempt a reservation without waiting; unlock only on success.
 * Always succeeds under the bare-metal port's single-context contract.
 */
bool pf_sd_card_try_lock(void);
void pf_sd_card_unlock(void);

#define PF_SD_CARD_BLOCK_SIZE 512u

typedef enum {
    PF_SD_CARD_OK = 0,
    PF_SD_CARD_INVALID_ARGUMENT,
    PF_SD_CARD_CONFIG_LOCKED,
    PF_SD_CARD_NOT_CONFIGURED,
    PF_SD_CARD_INIT_FAILED,
    PF_SD_CARD_NOT_READY,
    PF_SD_CARD_IO_ERROR,
} pf_sd_card_result_t;

typedef enum {
    PF_SD_CARD_OWNER_FIRMWARE = 0,
    PF_SD_CARD_OWNER_USB_MSC,
} pf_sd_card_owner_t;

typedef enum {
    PF_SD_CARD_TRANSPORT_SDIO_PIO = 0,
    PF_SD_CARD_TRANSPORT_SPI,
} pf_sd_card_transport_t;

typedef struct {
    bool initialized;
    bool presence_known;
    bool present;
    bool cid_valid;
    uint32_t bus_clock_hz;
    uint32_t block_count;
    uint8_t cid[16];
} pf_sd_card_info_t;

typedef enum {
    /* Inclusive instrumentation windows, NOT exclusive latency phases.
     * DATA includes native TX response/busy and overlapped encode/decode;
     * STOP includes command/busy, RECOVERY includes initialization windows.
     * Never sum elapsed_us or compare phase values across transports directly.
     * See docs/sdcard-design.md, Profile Measurement Boundaries. ABI preserved. */
    PF_SD_CARD_PROFILE_COMMAND = 0,
    PF_SD_CARD_PROFILE_DATA,
    PF_SD_CARD_PROFILE_CRC,
    PF_SD_CARD_PROFILE_BUSY,
    PF_SD_CARD_PROFILE_STOP,
    PF_SD_CARD_PROFILE_RECOVERY,
    PF_SD_CARD_PROFILE_PHASE_COUNT,
} pf_sd_card_profile_phase_t;

typedef enum {
    PF_SD_CARD_PROFILE_CRC_FAILURE = 0,
    PF_SD_CARD_PROFILE_BATCH_FALLBACK,
    PF_SD_CARD_PROFILE_TIMEOUT,
    PF_SD_CARD_PROFILE_RECOVERY_ATTEMPT,
    PF_SD_CARD_PROFILE_EVENT_COUNT,
} pf_sd_card_profile_event_t;

/** Serialized-driver profiling; phase timings overlap and must not be summed. */
typedef struct {
    uint64_t elapsed_us[PF_SD_CARD_PROFILE_PHASE_COUNT];
    uint32_t calls[PF_SD_CARD_PROFILE_PHASE_COUNT];
    uint32_t events[PF_SD_CARD_PROFILE_EVENT_COUNT];
} pf_sd_card_profile_t;

typedef enum {
    PF_SD_CARD_OPERATION_NONE = 0,
    PF_SD_CARD_OPERATION_INIT,
    PF_SD_CARD_OPERATION_READ,
    PF_SD_CARD_OPERATION_WRITE,
    PF_SD_CARD_OPERATION_SYNC,
    PF_SD_CARD_OPERATION_MSC_ACQUIRE,
    PF_SD_CARD_OPERATION_MSC_RELEASE,
} pf_sd_card_operation_t;

/** Last facade failure and the transport context captured when it occurred. */
typedef struct {
    uint32_t sequence;
    pf_sd_card_operation_t operation;
    pf_sd_card_result_t result;
    pf_sd_card_owner_t owner;
    pf_sd_card_transport_t transport;
    uint32_t transport_init_stage;
    pf_sd_card_profile_t profile;
} pf_sd_card_diagnostics_t;

/** Enable optional profiling, disabled at boot; serialize with all card access. */
void pf_sd_card_set_profiling_enabled(bool enabled);
/** Clear accumulated metrics without changing profiling enablement. */
void pf_sd_card_reset_profile(void);
/** Copy metrics to caller-owned storage; NULL is ignored. Requires serialized access. */
void pf_sd_card_get_profile(pf_sd_card_profile_t *profile);
/**
 * Snapshot the most recent facade failure.
 *
 * Operation NONE means no failure is retained. A zero sequence means no failure
 * has occurred since boot; clearing preserves the monotonic sequence.
 */
void pf_sd_card_get_diagnostics(pf_sd_card_diagnostics_t *diagnostics);
/** Clear the retained failure while preserving its monotonic sequence number. */
void pf_sd_card_clear_diagnostics(void);

/** Return whether the selected card transport has initialized successfully. */
bool pf_sd_card_is_initialized(void);
/** Serialized storage owner notification after debounced card removal. */
void pf_sd_card_confirm_removed(void);

/** Return the initialized card's number of 512-byte sectors, or zero if unavailable. */
uint32_t pf_sd_card_block_count(void);

/** Read 512-byte sectors into a caller-owned buffer. */
pf_sd_card_result_t pf_sd_card_read_blocks(uint32_t first_block, void *buffer,
                                           uint32_t block_count);

/** Write 512-byte sectors from a caller-owned buffer. */
pf_sd_card_result_t pf_sd_card_write_blocks(uint32_t first_block, const void *buffer,
                                            uint32_t block_count);

/** Complete any pending write. */
pf_sd_card_result_t pf_sd_card_sync(void);

/** Snapshot initialized card telemetry; presence is unknown until detect or init succeeds. */
void pf_sd_card_get_info(pf_sd_card_info_t *info);

/** Return the selected transport name (sdio, spi, or unconfigured).
 * The returned string has static lifetime and must not be modified.
 */
const char *pf_sd_card_transport_name(void);

/**
 * Set a callback run between polls of card waits that can last milliseconds
 * (write busy, command-line busy). Keep it short, such as taskYIELD(); NULL selects
 * plain busy polling. Set it before the first transfer; it is not synchronized.
 */
void pf_sd_card_set_idle_hook(void (*hook)(void));

#endif