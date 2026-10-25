/** @file
 * @brief Application SD lifecycle, diagnostics and exclusive firmware/USB ownership.
 */
#ifndef PF_SD_MEDIA_H
#define PF_SD_MEDIA_H

#include "drivers/sdcard/sd.h"
#include "middleware/storage/block_device.h"

/** Exclusive application access identity; not a card protocol property. */
typedef enum {
    PF_SD_MEDIA_OWNER_FIRMWARE = 0, /**< Firmware filesystem access. */
    PF_SD_MEDIA_OWNER_USB_MSC,      /**< USB host block access. */
} pf_sd_media_owner_t;

/** Last application operation failure, retaining the historical CLI operation encoding. */
typedef enum {
    PF_SD_MEDIA_OPERATION_NONE = 0, /**< No retained failure. */
    PF_SD_MEDIA_OPERATION_INIT,     /**< Card initialization. */
    PF_SD_MEDIA_OPERATION_READ,     /**< Owner-gated read. */
    PF_SD_MEDIA_OPERATION_WRITE,    /**< Owner-gated write. */
    PF_SD_MEDIA_OPERATION_SYNC,     /**< Owner-gated sync. */
    PF_SD_MEDIA_OPERATION_MSC_ACQUIRE, /**< USB ownership acquisition. */
    PF_SD_MEDIA_OPERATION_MSC_RELEASE, /**< USB ownership release. */
} pf_sd_media_operation_t;

/** Application failure snapshot, serialized by the shared SD reservation. */
typedef struct {
    uint32_t sequence; /**< Monotonic failure sequence; clear does not reset it. */
    pf_sd_media_operation_t operation; /**< Operation that failed. */
    pf_sd_result_t result; /**< Driver result or ownership rejection. */
    pf_sd_media_owner_t owner; /**< Owner when the failure occurred. */
    pf_sd_transport_t transport; /**< Build-selected protocol transport. */
    uint32_t transport_init_stage; /**< Driver initialization stage at failure. */
    pf_sd_profile_t profile; /**< Driver metrics at failure. */
} pf_sd_media_diagnostics_t;

/** Obtain board SD configuration and initialize the execution port before task startup.
 * Returns OK, NOT_CONFIGURED for an unavailable board configuration, or IO_ERROR for port setup failure.
 * Unavailability and errors are logged; setup never accesses card data.
 */
pf_sd_result_t pf_sd_media_setup(void);
/** Initialize configured firmware media; reject USB ownership and mounted volumes.
 * Reinitialization is explicit and never formats or replays failed transfers.
 */
pf_sd_result_t pf_sd_media_initialize(void);
/** Query the current owner under the shared reservation. */
pf_sd_media_owner_t pf_sd_media_get_owner(void);
/** Consume the remount/reinitialization request published by USB release. */
bool pf_sd_media_take_reinit_request(void);
/** Read sectors under firmware ownership, without retries. */
pf_sd_result_t pf_sd_media_read_blocks(uint32_t first, void *buffer, uint32_t count);
/** Write sectors under firmware ownership, without retries. */
pf_sd_result_t pf_sd_media_write_blocks(uint32_t first, const void *buffer, uint32_t count);
/** Check durability under firmware ownership; never replay failed writes. */
pf_sd_result_t pf_sd_media_sync(void);
/** Acquire initialized unmounted media for USB.
 * Caller reserves SD across worker-idle checks and handoff, preventing worker admission.
 */
pf_sd_result_t pf_sd_media_acquire_usb(void);
/** Release USB after caller-checked eject/sync; does not flush.
 * Caller reserves SD and prevents USB I/O; confirmed removal is an explicit no-flush exception.
 */
pf_sd_result_t pf_sd_media_release_usb(void);
/** Release faulted USB media after detach, never claiming a successful flush.
 * Caller reserves SD and queues recovery before releasing the reservation. Rejects healthy media.
 */
pf_sd_result_t pf_sd_media_release_usb_for_recovery(void);
/** Read sectors under USB ownership, without retries. */
pf_sd_result_t pf_sd_media_usb_read_blocks(uint32_t first, void *buffer, uint32_t count);
/** Write sectors under USB ownership, without retries. */
pf_sd_result_t pf_sd_media_usb_write_blocks(uint32_t first, const void *buffer, uint32_t count);
/** Sync under USB ownership. */
pf_sd_result_t pf_sd_media_usb_sync(void);
/** Borrow the static firmware-gated block descriptor for FatFs. */
const pf_block_device_t *pf_sd_media_firmware_device(void);
/** Copy retained application failure diagnostics; NULL is ignored. */
void pf_sd_media_get_diagnostics(pf_sd_media_diagnostics_t *diagnostics);
/** Clear retained failure, preserving the sequence. */
void pf_sd_media_clear_diagnostics(void);

#endif
