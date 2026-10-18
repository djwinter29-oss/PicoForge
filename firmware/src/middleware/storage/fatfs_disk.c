/** @file
 * @brief Adapt FatFs drive-zero callbacks to the bound logical block device.
 */
#include "ff.h"
#include "diskio.h"
#include "fatfs_disk.h"

#include <stddef.h>
#include <stdint.h>

static const pf_block_device_t *disk_device;

void pf_fatfs_disk_bind(const pf_block_device_t *device)
{
    disk_device = device;
}

static DRESULT translate_result(pf_block_result_t result)
{
    if (result == PF_BLOCK_OK) {
        return RES_OK;
    }
    if (result == PF_BLOCK_NOT_READY) {
        return RES_NOTRDY;
    }
    if (result == PF_BLOCK_WRITE_PROTECTED) {
        return RES_WRPRT;
    }
    if (result == PF_BLOCK_INVALID_ARGUMENT) {
        return RES_PARERR;
    }
    return RES_ERROR;
}

static bool valid_request(BYTE drive, const void *buffer, LBA_t sector, UINT count)
{
    return (drive == 0u) && (buffer != NULL) && (count != 0u) &&
           ((uint64_t)sector <= UINT32_MAX) && ((uint64_t)count <= UINT32_MAX) &&
           ((uint64_t)sector + (uint64_t)count <= (uint64_t)UINT32_MAX + 1u);
}

DSTATUS disk_initialize(BYTE drive)
{
    if (drive != 0u) {
        return STA_NOINIT;
    }
    pf_block_geometry_t geometry;
    if (disk_device == NULL || pf_block_geometry(disk_device, &geometry) != PF_BLOCK_OK ||
        geometry.sector_size != FF_MIN_SS || geometry.sector_size != FF_MAX_SS) {
        return STA_NOINIT;
    }
    return geometry.write_protected ? STA_PROTECT : 0u;
}

DSTATUS disk_status(BYTE drive)
{
    return disk_initialize(drive);
}

DRESULT disk_read(BYTE drive, BYTE *buffer, LBA_t sector, UINT count)
{
    if (!valid_request(drive, buffer, sector, count)) {
        return RES_PARERR;
    }
    if ((disk_status(drive) & STA_NOINIT) != 0u) {
        return RES_NOTRDY;
    }

    return translate_result(pf_block_read(disk_device, (uint32_t)sector, buffer, (uint32_t)count));
}

DRESULT disk_write(BYTE drive, const BYTE *buffer, LBA_t sector, UINT count)
{
    if (!valid_request(drive, buffer, sector, count)) {
        return RES_PARERR;
    }
    if ((disk_status(drive) & STA_NOINIT) != 0u) {
        return RES_NOTRDY;
    }

    return translate_result(pf_block_write(disk_device, (uint32_t)sector, buffer, (uint32_t)count));
}

DRESULT disk_ioctl(BYTE drive, BYTE command, void *buffer)
{
    if (drive != 0u) {
        return RES_PARERR;
    }
    if ((disk_status(drive) & STA_NOINIT) != 0u) {
        return RES_NOTRDY;
    }

    pf_block_geometry_t geometry;
    if (pf_block_geometry(disk_device, &geometry) != PF_BLOCK_OK) {
        return RES_NOTRDY;
    }
    switch (command) {
    case CTRL_SYNC:
        return translate_result(pf_block_sync(disk_device));
    case GET_SECTOR_COUNT:
        if (buffer == NULL) {
            return RES_PARERR;
        }
        *(LBA_t *)buffer = (LBA_t)geometry.sector_count;
        return RES_OK;
    case GET_SECTOR_SIZE:
        if (buffer == NULL) {
            return RES_PARERR;
        }
        *(WORD *)buffer = (WORD)geometry.sector_size;
        return RES_OK;
    case GET_BLOCK_SIZE:
        if (buffer == NULL) {
            return RES_PARERR;
        }
        *(DWORD *)buffer = geometry.erase_sector_count;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}