#include "ff.h"
#include "diskio.h"
#include "sdio_block.h"

#include <stdint.h>
#include <string.h>

static BYTE unaligned_sector_buffer[PICOFORGE_SDIO_BLOCK_SIZE] __attribute__((aligned(4)));

static DRESULT translate_result(picoforge_sdio_result_t result)
{
    if (result == PICOFORGE_SDIO_OK) {
        return RES_OK;
    }
    if (result == PICOFORGE_SDIO_NOT_READY) {
        return RES_NOTRDY;
    }
    if (result == PICOFORGE_SDIO_INVALID_ARGUMENT) {
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
    return picoforge_sdio_is_initialized() ? 0u : STA_NOINIT;
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
    if (!picoforge_sdio_is_initialized()) {
        return RES_NOTRDY;
    }

    if (((uintptr_t)buffer & 3u) == 0u) {
        return translate_result(picoforge_sdio_read_blocks((uint32_t)sector, buffer, (uint32_t)count));
    }

    for (UINT index = 0u; index < count; index++) {
        picoforge_sdio_result_t result = picoforge_sdio_read_blocks(
            (uint32_t)sector + index, unaligned_sector_buffer, 1u);
        if (result != PICOFORGE_SDIO_OK) {
            return translate_result(result);
        }
        memcpy(buffer + (size_t)index * PICOFORGE_SDIO_BLOCK_SIZE,
               unaligned_sector_buffer, PICOFORGE_SDIO_BLOCK_SIZE);
    }
    return RES_OK;
}

DRESULT disk_write(BYTE drive, const BYTE *buffer, LBA_t sector, UINT count)
{
    if (!valid_request(drive, buffer, sector, count)) {
        return RES_PARERR;
    }
    if (!picoforge_sdio_is_initialized()) {
        return RES_NOTRDY;
    }

    if (((uintptr_t)buffer & 3u) == 0u) {
        return translate_result(picoforge_sdio_write_blocks((uint32_t)sector, buffer, (uint32_t)count));
    }

    for (UINT index = 0u; index < count; index++) {
        memcpy(unaligned_sector_buffer,
               buffer + (size_t)index * PICOFORGE_SDIO_BLOCK_SIZE,
               PICOFORGE_SDIO_BLOCK_SIZE);
        picoforge_sdio_result_t result = picoforge_sdio_write_blocks(
            (uint32_t)sector + index, unaligned_sector_buffer, 1u);
        if (result != PICOFORGE_SDIO_OK) {
            return translate_result(result);
        }
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE drive, BYTE command, void *buffer)
{
    if (drive != 0u) {
        return RES_PARERR;
    }
    if (!picoforge_sdio_is_initialized()) {
        return RES_NOTRDY;
    }

    switch (command) {
    case CTRL_SYNC:
        return translate_result(picoforge_sdio_sync());
    case GET_SECTOR_COUNT:
        if (buffer == NULL) {
            return RES_PARERR;
        }
        *(LBA_t *)buffer = (LBA_t)picoforge_sdio_block_count();
        return RES_OK;
    case GET_SECTOR_SIZE:
        if (buffer == NULL) {
            return RES_PARERR;
        }
        *(WORD *)buffer = (WORD)PICOFORGE_SDIO_BLOCK_SIZE;
        return RES_OK;
    case GET_BLOCK_SIZE:
        if (buffer == NULL) {
            return RES_PARERR;
        }
        *(DWORD *)buffer = 1u;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}