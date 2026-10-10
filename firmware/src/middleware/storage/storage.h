#ifndef PICOFORGE_STORAGE_H
#define PICOFORGE_STORAGE_H

#include <stdbool.h>

#include "ff.h"
#include "sdio_pio.h"

#define PICOFORGE_STORAGE_VOLUME "0:"

typedef enum {
    PICOFORGE_STORAGE_OK = 0,
    PICOFORGE_STORAGE_INVALID_ARGUMENT,
    PICOFORGE_STORAGE_ALREADY_MOUNTED,
    PICOFORGE_STORAGE_NOT_MOUNTED,
    PICOFORGE_STORAGE_SDIO_ERROR,
    PICOFORGE_STORAGE_FILESYSTEM_ERROR,
} picoforge_storage_result_t;

/**
 * Initialize the SD card and mount its existing FAT/exFAT volume.
 *
 * This never formats media. Serialize this call and all FatFs operations from
 * one storage-owner task because FatFs is configured without cross-task locking.
 * The optional result pointer receives the underlying FatFs result code.
 */
picoforge_storage_result_t picoforge_storage_mount(
    const picoforge_sdio_config_t *sdio_config, FRESULT *filesystem_result);

/** Unmount the volume after all open FatFs objects have been closed. */
picoforge_storage_result_t picoforge_storage_unmount(FRESULT *filesystem_result);

/** Return whether the FatFs volume is currently mounted. */
bool picoforge_storage_is_mounted(void);

#endif