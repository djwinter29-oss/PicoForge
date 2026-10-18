/** @file
 * @brief Application FatFs volume instance, drive-zero binding and mount telemetry.
 */
#include "fatfs_volume.h"
#include "middleware/storage/fatfs_disk.h"

#include <stdatomic.h>
#include <stddef.h>

static FATFS storage_filesystem; /**< FatFs state owned by the serialized volume service. */
static bool storage_mounted; /**< Registration state; caller serialization is required. */
static atomic_int storage_format = ATOMIC_VAR_INIT(PF_FATFS_VOLUME_FILESYSTEM_UNKNOWN); /**< Atomic telemetry cache. */

/** @brief Translate FatFs format metadata into the public telemetry values. */
static pf_fatfs_volume_filesystem_format_t storage_format_from_fatfs(BYTE fs_type)
{
    switch (fs_type) {
    case FS_FAT12: return PF_FATFS_VOLUME_FILESYSTEM_FAT12;
    case FS_FAT16: return PF_FATFS_VOLUME_FILESYSTEM_FAT16;
    case FS_FAT32: return PF_FATFS_VOLUME_FILESYSTEM_FAT32;
    case FS_EXFAT: return PF_FATFS_VOLUME_FILESYSTEM_EXFAT;
    default: return PF_FATFS_VOLUME_FILESYSTEM_UNKNOWN;
    }
}

pf_fatfs_volume_result_t pf_fatfs_volume_mount(
    const pf_block_device_t *device, FRESULT *filesystem_result)
{
    FRESULT mount_result;

    if (filesystem_result != NULL) {
        *filesystem_result = FR_OK;
    }
    if (device == NULL) {
        return PF_FATFS_VOLUME_INVALID_ARGUMENT;
    }
    if (storage_mounted) {
        return PF_FATFS_VOLUME_ALREADY_MOUNTED;
    }

    atomic_store(&storage_format, PF_FATFS_VOLUME_FILESYSTEM_UNKNOWN);

    pf_fatfs_disk_bind(device);
    mount_result = f_mount(&storage_filesystem, PF_FATFS_VOLUME_PATH, 1u);
    if (filesystem_result != NULL) {
        *filesystem_result = mount_result;
    }
    if (mount_result != FR_OK) {
        (void)f_mount(NULL, PF_FATFS_VOLUME_PATH, 0u);
        pf_fatfs_disk_bind(NULL);
        return PF_FATFS_VOLUME_FILESYSTEM_ERROR;
    }

    storage_mounted = true;
    atomic_store(&storage_format, storage_format_from_fatfs(storage_filesystem.fs_type));
    return PF_FATFS_VOLUME_OK;
}

pf_fatfs_volume_result_t pf_fatfs_volume_unmount(FRESULT *filesystem_result)
{
    FRESULT unmount_result;

    if (filesystem_result != NULL) {
        *filesystem_result = FR_OK;
    }
    if (!storage_mounted) {
        return PF_FATFS_VOLUME_NOT_MOUNTED;
    }

    unmount_result = f_mount(NULL, PF_FATFS_VOLUME_PATH, 0u);
    if (filesystem_result != NULL) {
        *filesystem_result = unmount_result;
    }
    if (unmount_result != FR_OK) {
        return PF_FATFS_VOLUME_FILESYSTEM_ERROR;
    }

    storage_mounted = false;
    pf_fatfs_disk_bind(NULL);
    atomic_store(&storage_format, PF_FATFS_VOLUME_FILESYSTEM_UNKNOWN);
    return PF_FATFS_VOLUME_OK;
}

bool pf_fatfs_volume_is_mounted(void)
{
    return storage_mounted;
}

pf_fatfs_volume_filesystem_format_t pf_fatfs_volume_filesystem_format(void)
{
    return (pf_fatfs_volume_filesystem_format_t)atomic_load(&storage_format);
}

void pf_fatfs_volume_forget_filesystem_format(void)
{
    atomic_store(&storage_format, PF_FATFS_VOLUME_FILESYSTEM_UNKNOWN);
}