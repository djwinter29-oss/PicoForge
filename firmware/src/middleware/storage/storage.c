#include "storage.h"

static FATFS storage_filesystem;
static bool storage_mounted;

pf_storage_result_t pf_storage_mount(
    const pf_sdio_config_t *sdio_config, FRESULT *filesystem_result)
{
    pf_sdio_result_t sdio_result;
    FRESULT mount_result;

    if (filesystem_result != NULL) {
        *filesystem_result = FR_OK;
    }
    if (sdio_config == NULL) {
        return PF_STORAGE_INVALID_ARGUMENT;
    }
    if (storage_mounted) {
        return PF_STORAGE_ALREADY_MOUNTED;
    }

    sdio_result = pf_sdio_init(sdio_config);
    if (sdio_result != PF_SDIO_OK) {
        return PF_STORAGE_SDIO_ERROR;
    }

    mount_result = f_mount(&storage_filesystem, PF_STORAGE_VOLUME, 1u);
    if (filesystem_result != NULL) {
        *filesystem_result = mount_result;
    }
    if (mount_result != FR_OK) {
        (void)f_mount(NULL, PF_STORAGE_VOLUME, 0u);
        return PF_STORAGE_FILESYSTEM_ERROR;
    }

    storage_mounted = true;
    return PF_STORAGE_OK;
}

pf_storage_result_t pf_storage_unmount(FRESULT *filesystem_result)
{
    FRESULT unmount_result;

    if (filesystem_result != NULL) {
        *filesystem_result = FR_OK;
    }
    if (!storage_mounted) {
        return PF_STORAGE_NOT_MOUNTED;
    }

    unmount_result = f_mount(NULL, PF_STORAGE_VOLUME, 0u);
    if (filesystem_result != NULL) {
        *filesystem_result = unmount_result;
    }
    if (unmount_result != FR_OK) {
        return PF_STORAGE_FILESYSTEM_ERROR;
    }

    storage_mounted = false;
    return PF_STORAGE_OK;
}

bool pf_storage_is_mounted(void)
{
    return storage_mounted;
}