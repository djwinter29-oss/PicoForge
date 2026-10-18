/** @file
 * @brief Application singleton FatFs volume over a caller-selected logical block device.
 */
#ifndef PF_FATFS_VOLUME_H
#define PF_FATFS_VOLUME_H

#include <stdbool.h>

#include "ff.h"
#include "middleware/storage/block_device.h"

/** FatFs logical volume managed by this service. */
#define PF_FATFS_VOLUME_PATH "0:"

/** Volume lifecycle outcome; the optional FRESULT supplies filesystem error details. */
typedef enum {
    PF_FATFS_VOLUME_OK = 0,            /**< Requested transition completed. */
    PF_FATFS_VOLUME_INVALID_ARGUMENT, /**< Missing block-device descriptor. */
    PF_FATFS_VOLUME_ALREADY_MOUNTED,  /**< Mount rejected because the volume is already registered. */
    PF_FATFS_VOLUME_NOT_MOUNTED,      /**< Unmount rejected because no volume is registered. */
    PF_FATFS_VOLUME_FILESYSTEM_ERROR, /**< FatFs mount or unregister failed. */
} pf_fatfs_volume_result_t;

/** Cached filesystem format for telemetry; unknown when invalidated or unmounted. */
typedef enum {
    PF_FATFS_VOLUME_FILESYSTEM_UNKNOWN = 0, /**< No currently cached format. */
    PF_FATFS_VOLUME_FILESYSTEM_FAT12,       /**< FAT12 volume. */
    PF_FATFS_VOLUME_FILESYSTEM_FAT16,       /**< FAT16 volume. */
    PF_FATFS_VOLUME_FILESYSTEM_FAT32,       /**< FAT32 volume. */
    PF_FATFS_VOLUME_FILESYSTEM_EXFAT,       /**< exFAT volume. */
} pf_fatfs_volume_filesystem_format_t;

/**
 * @brief Bind an initialized logical block device and mount its existing FAT/exFAT volume.
 *
 * This never formats media. Serialize this call and all FatFs operations from
 * one storage-owner task because FatFs is configured without cross-task locking.
 * Mount-state telemetry is atomic, but does not serialize lifecycle or filesystem operations.
 * Device initialization and ownership transitions remain external. The borrowed descriptor, operations, and context
 * must outlive the mount and remain valid until successful unmount or failed-mount cleanup.
 * @param device Logical block device; this call does not initialize its backing media.
 * @param filesystem_result Optional FatFs result; FR_OK when no filesystem error occurred.
 * @return Volume lifecycle outcome; backend diagnostics remain with the device implementation.
 */
pf_fatfs_volume_result_t pf_fatfs_volume_mount(
    const pf_block_device_t *device, FRESULT *filesystem_result);

/** @brief Unregister the volume after all open FatFs objects have been closed.
 * This is not a flush: callers must check f_sync/f_close before unmount and
 * ownership handoff. Removal/fault cleanup may unregister invalid media without
 * syncing; that path must never be reported as a successful flush.
 * Serialize with mount and all FatFs operations.
 * @param filesystem_result Optional FatFs unregister result; FR_OK if not mounted.
 * @return Volume lifecycle outcome; failed unregister retains the binding and mounted state.
 */
pf_fatfs_volume_result_t pf_fatfs_volume_unmount(FRESULT *filesystem_result);

/** @brief Query registration state, not physical media presence or current I/O access.
 * @return Atomic snapshot of the last completed mount/unmount transition.
 * This grants no filesystem access; check-and-act operations still require external serialization.
 */
bool pf_fatfs_volume_is_mounted(void);

/** @brief Read the atomic telemetry snapshot without taking the storage-owner reservation.
 * @return Last mounted format, or unknown after unmount, invalidation, or before mounting.
 * This snapshot grants no media access and is not a mount-state synchronization mechanism.
 */
pf_fatfs_volume_filesystem_format_t pf_fatfs_volume_filesystem_format(void);

/** @brief Atomically invalidate the cached format when external ownership may modify the volume. */
void pf_fatfs_volume_forget_filesystem_format(void);

#endif