#include "storage.h"

static FATFS storage_filesystem;
static bool storage_mounted;

picoforge_storage_result_t picoforge_storage_mount(
    const picoforge_sdio_config_t *sdio_config, FRESULT *filesystem_result)
{
    picoforge_sdio_result_t sdio_result;
    FRESULT mount_result;

    if (filesystem_result != NULL) {
        *filesystem_result = FR_OK;
    }
    if (sdio_config == NULL) {
        return PICOFORGE_STORAGE_INVALID_ARGUMENT;
    }
    if (storage_mounted) {
        return PICOFORGE_STORAGE_ALREADY_MOUNTED;
    }

    sdio_result = picoforge_sdio_init(sdio_config);
    if (sdio_result != PICOFORGE_SDIO_OK) {
        return PICOFORGE_STORAGE_SDIO_ERROR;
    }

    mount_result = f_mount(&storage_filesystem, PICOFORGE_STORAGE_VOLUME, 1u);
    if (filesystem_result != NULL) {
        *filesystem_result = mount_result;
    }
    if (mount_result != FR_OK) {
        (void)f_mount(NULL, PICOFORGE_STORAGE_VOLUME, 0u);
        return PICOFORGE_STORAGE_FILESYSTEM_ERROR;
    }

    storage_mounted = true;
    return PICOFORGE_STORAGE_OK;
}

picoforge_storage_result_t picoforge_storage_unmount(FRESULT *filesystem_result)
{
    FRESULT unmount_result;

    if (filesystem_result != NULL) {
        *filesystem_result = FR_OK;
    }
    if (!storage_mounted) {
        return PICOFORGE_STORAGE_NOT_MOUNTED;
    }

    unmount_result = f_mount(NULL, PICOFORGE_STORAGE_VOLUME, 0u);
    if (filesystem_result != NULL) {
        *filesystem_result = unmount_result;
    }
    if (unmount_result != FR_OK) {
        return PICOFORGE_STORAGE_FILESYSTEM_ERROR;
    }

    storage_mounted = false;
    return PICOFORGE_STORAGE_OK;
}

bool picoforge_storage_is_mounted(void)
{
    return storage_mounted;
}