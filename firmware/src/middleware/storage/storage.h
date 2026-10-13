#ifndef PF_STORAGE_H
#define PF_STORAGE_H

#include <stdbool.h>

#include "ff.h"

typedef struct pf_sd_card_config pf_sd_card_config_t;

#define PF_STORAGE_VOLUME "0:"

typedef enum {
    PF_STORAGE_OK = 0,
    PF_STORAGE_INVALID_ARGUMENT,
    PF_STORAGE_ALREADY_MOUNTED,
    PF_STORAGE_NOT_MOUNTED,
    PF_STORAGE_SD_CARD_ERROR,
    PF_STORAGE_FILESYSTEM_ERROR,
} pf_storage_result_t;

typedef enum {
    PF_STORAGE_FILESYSTEM_UNKNOWN = 0,
    PF_STORAGE_FILESYSTEM_FAT12,
    PF_STORAGE_FILESYSTEM_FAT16,
    PF_STORAGE_FILESYSTEM_FAT32,
    PF_STORAGE_FILESYSTEM_EXFAT,
} pf_storage_filesystem_format_t;

/**
 * Initialize the SD card and mount its existing FAT/exFAT volume.
 *
 * This never formats media. Serialize this call and all FatFs operations from
 * one storage-owner task because FatFs is configured without cross-task locking.
 * The optional result pointer receives the underlying FatFs result code.
 */
pf_storage_result_t pf_storage_mount(
    const pf_sd_card_config_t *sd_card_config, FRESULT *filesystem_result);

/** Unmount the volume after all open FatFs objects have been closed. */
pf_storage_result_t pf_storage_unmount(FRESULT *filesystem_result);

/** Return whether the FatFs volume is currently mounted. */
bool pf_storage_is_mounted(void);

/** Return the last successfully mounted filesystem format, or unknown before mount. */
pf_storage_filesystem_format_t pf_storage_filesystem_format(void);

/** Forget the cached format when external ownership may modify the volume. */
void pf_storage_forget_filesystem_format(void);

#endif