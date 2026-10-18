/** @file
 * @brief Application-owned external NOR lifecycle and exclusive firmware/USB access.
 */
#ifndef PF_NOR_MEDIA_H
#define PF_NOR_MEDIA_H

#include "middleware/storage/block_device.h"

/** Application operation associated with a retained NOR failure. */
typedef enum {
    PF_NOR_MEDIA_OPERATION_NONE = 0, /**< No recorded failure. */
    PF_NOR_MEDIA_OPERATION_PREPARE, /**< Board serial-resource preparation. */
    PF_NOR_MEDIA_OPERATION_INITIALIZE, /**< Transport/flash initialization or quad enable. */
    PF_NOR_MEDIA_OPERATION_OPEN, /**< LevelX region open. */
    PF_NOR_MEDIA_OPERATION_CLOSE, /**< LevelX close before recovery. */
    PF_NOR_MEDIA_OPERATION_READ, /**< Logical block read. */
    PF_NOR_MEDIA_OPERATION_WRITE, /**< Logical block write. */
    PF_NOR_MEDIA_OPERATION_SYNC, /**< Logical block sync. */
} pf_nor_media_operation_t;

/** Retained NOR failure plus current access state; vendor codes remain numeric diagnostics. */
typedef struct {
    uint32_t sequence; /**< Failure counter; successful recovery does not clear it. */
    pf_nor_media_operation_t operation; /**< Operation that failed, or NONE before any failure. */
    bool failure_usb_owned; /**< Owner at failure time, independent of current ownership. */
    pf_block_result_t result; /**< Last failed application operation result. */
    uint32_t raw_result; /**< Raw NOR failure code. */
    uint32_t levelx_result; /**< Open/close code; block adapter reports synthetic LX_ERROR on failure. */
    uint32_t system_error; /**< LevelX system-error callback code. */
    bool ready; /**< Current initialized, open and nonfaulted state. */
    bool usb_owned; /**< Current exclusive owner is USB. */
} pf_nor_media_diagnostics_t;

/** @brief Copy retained failure and current state under the media reservation.
 * @param diagnostics Non-NULL destination owned by the caller.
 */
void pf_nor_media_get_diagnostics(pf_nor_media_diagnostics_t *diagnostics);

/** @brief Initialize the reservation before starting the application tasks. */
bool pf_nor_media_setup(void);
/** @brief Reserve the selected NOR volume; recursive for nested synchronous block calls. */
void pf_nor_media_lock(void);
/** @brief Try to reserve without blocking the USB task. */
bool pf_nor_media_try_lock(void);
/** @brief Release one recursive reservation. */
void pf_nor_media_unlock(void);
/** @brief Initialize/recover only the explicitly configured external LevelX region.
 * Opening may update LevelX metadata. Never formats FAT or erases the entire region.
 * @return Explicit media status; detailed raw/LevelX errors are logged.
 */
pf_block_result_t pf_nor_media_initialize(void);
/** @brief Query open, nonfaulted media, independently of current owner. */
bool pf_nor_media_ready(void);
/** @brief Get logical capacity, or zero when unavailable. */
uint32_t pf_nor_media_sector_count(void);
/** @brief Borrow the firmware-gated descriptor.
 * Static descriptors and context remain alive for the application lifetime.
 */
const pf_block_device_t *pf_nor_media_firmware_device(void);
/** @brief Borrow the USB-gated descriptor with application lifetime. */
const pf_block_device_t *pf_nor_media_usb_device(void);
/** @brief Acquire USB ownership; checks ready media and an unmounted volume.
 * Caller holds the reservation across the worker-idle check and this call, and
 * prevents worker admission during handoff. This operation does not check worker state.
 */
pf_block_result_t pf_nor_media_acquire_usb(void);
/** @brief Return ownership after caller-checked host eject and successful sync.
 * Caller holds the reservation and prevents new USB I/O. Does not sync or claim durability.
 */
pf_block_result_t pf_nor_media_release_usb(void);
/** @brief Return faulted USB media after detach, without sync or a clean-eject claim.
 * Rejects ready media. Caller reserves media, blocks worker admission and detaches USB
 * before this call; the queued worker performs recovery after the reservation is released.
 */
pf_block_result_t pf_nor_media_release_usb_for_recovery(void);
/** @brief Query whether USB owns the media. */
bool pf_nor_media_usb_owned(void);
/** @brief Consume the remount request published by a successful USB ownership release. */
bool pf_nor_media_take_remount_request(void);

#endif
