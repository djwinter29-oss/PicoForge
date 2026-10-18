/** @file
 * @brief Single FatFs volume lifecycle, currently backed by the singleton SD card.
 */
#ifndef PF_FATFS_VOLUME_H
#define PF_FATFS_VOLUME_H

#include <stdbool.h>

#include "ff.h"

/** SD configuration supplied by the caller; initialization remains SD-specific. */
typedef struct pf_sd_config pf_sd_config_t;

/** FatFs logical volume managed by this service. */
#define PF_STORAGE_VOLUME "0:"

/** Volume lifecycle outcome; the optional FRESULT supplies filesystem error details. */
typedef enum {
    PF_STORAGE_OK = 0,            /**< Requested transition completed. */
    PF_STORAGE_INVALID_ARGUMENT, /**< Missing SD configuration. */
    PF_STORAGE_ALREADY_MOUNTED,  /**< Mount rejected because the volume is already registered. */
    PF_STORAGE_NOT_MOUNTED,      /**< Unmount rejected because no volume is registered. */
    PF_STORAGE_SD_CARD_ERROR,    /**< SD initialization failed before mounting. */
    PF_STORAGE_FILESYSTEM_ERROR, /**< FatFs mount or unregister failed. */
} pf_storage_result_t;

/** Cached filesystem format for telemetry; unknown when invalidated or unmounted. */
typedef enum {
    PF_STORAGE_FILESYSTEM_UNKNOWN = 0, /**< No currently cached format. */
    PF_STORAGE_FILESYSTEM_FAT12,       /**< FAT12 volume. */
    PF_STORAGE_FILESYSTEM_FAT16,       /**< FAT16 volume. */
    PF_STORAGE_FILESYSTEM_FAT32,       /**< FAT32 volume. */
    PF_STORAGE_FILESYSTEM_EXFAT,       /**< exFAT volume. */
} pf_storage_filesystem_format_t;

/**
 * @brief Initialize the SD card and mount its existing FAT/exFAT volume.
 *
 * This never formats media. Serialize this call and all FatFs operations from
 * one storage-owner task because FatFs is configured without cross-task locking.
 * Serialize mount-state queries with lifecycle changes too. This service adds no lock.
 * @param sd_card_config SD configuration passed to the existing card facade.
 * @param filesystem_result Optional FatFs result; FR_OK when no filesystem error occurred.
 * @return Volume lifecycle outcome; SD diagnostics remain in the SD facade.
 */
pf_storage_result_t pf_storage_mount(
    const pf_sd_config_t *sd_card_config, FRESULT *filesystem_result);

/** @brief Unregister the volume after all open FatFs objects have been closed.
 * This is not a flush: callers must check f_sync/f_close before unmount and
 * ownership handoff. Removal/fault cleanup may unregister invalid media without
 * syncing; that path must never be reported as a successful flush.
 * Serialize with mount, mount-state queries, and all FatFs operations.
 * @param filesystem_result Optional FatFs unregister result; FR_OK if not mounted.
 * @return Volume lifecycle outcome; failed unregister retains the binding and mounted state.
 */
pf_storage_result_t pf_storage_unmount(FRESULT *filesystem_result);

/** @brief Query registration state, not physical media presence or current I/O access.
 * @return Whether mounted. Caller must serialize with mount/unmount; this is not an atomic snapshot.
 */
bool pf_storage_is_mounted(void);

/** @brief Read the atomic telemetry snapshot without taking the storage-owner reservation.
 * @return Last mounted format, or unknown after unmount, invalidation, or before mounting.
 * This snapshot grants no media access and is not a mount-state synchronization mechanism.
 */
pf_storage_filesystem_format_t pf_storage_filesystem_format(void);

/** @brief Atomically invalidate the cached format when external ownership may modify the volume. */
void pf_storage_forget_filesystem_format(void);

#endif