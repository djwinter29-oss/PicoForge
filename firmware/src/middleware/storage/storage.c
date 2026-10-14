#include "storage.h"
#include "sd_card.h"

#include <stdatomic.h>

static FATFS storage_filesystem;
static bool storage_mounted;
static atomic_int storage_format = ATOMIC_VAR_INIT(PF_STORAGE_FILESYSTEM_UNKNOWN);

static pf_storage_filesystem_format_t storage_format_from_fatfs(BYTE fs_type)
{
    switch (fs_type) {
    case FS_FAT12: return PF_STORAGE_FILESYSTEM_FAT12;
    case FS_FAT16: return PF_STORAGE_FILESYSTEM_FAT16;
    case FS_FAT32: return PF_STORAGE_FILESYSTEM_FAT32;
    case FS_EXFAT: return PF_STORAGE_FILESYSTEM_EXFAT;
    default: return PF_STORAGE_FILESYSTEM_UNKNOWN;
    }
}

pf_storage_result_t pf_storage_mount(
    const pf_sd_card_config_t *sd_card_config, FRESULT *filesystem_result)
{
    pf_sd_card_result_t sd_card_result;
    FRESULT mount_result;

    if (filesystem_result != NULL) {
        *filesystem_result = FR_OK;
    }
    if (sd_card_config == NULL) {
        return PF_STORAGE_INVALID_ARGUMENT;
    }
    if (storage_mounted) {
        return PF_STORAGE_ALREADY_MOUNTED;
    }

    atomic_store(&storage_format, PF_STORAGE_FILESYSTEM_UNKNOWN);

    sd_card_result = pf_sd_card_init(sd_card_config);
    if (sd_card_result != PF_SD_CARD_OK) {
        return PF_STORAGE_SD_CARD_ERROR;
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
    atomic_store(&storage_format, storage_format_from_fatfs(storage_filesystem.fs_type));
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
    atomic_store(&storage_format, PF_STORAGE_FILESYSTEM_UNKNOWN);
    return PF_STORAGE_OK;
}

bool pf_storage_is_mounted(void)
{
    return storage_mounted;
}

pf_storage_filesystem_format_t pf_storage_filesystem_format(void)
{
    return (pf_storage_filesystem_format_t)atomic_load(&storage_format);
}

void pf_storage_forget_filesystem_format(void)
{
    atomic_store(&storage_format, PF_STORAGE_FILESYSTEM_UNKNOWN);
}