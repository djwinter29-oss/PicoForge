/** @file
 * @brief Sector batching over LevelX; stop at the first error without replay.
 */
#include "levelx_block_device.h"

#include <stddef.h>
#include <string.h>

/** @brief Distinguish unavailable control state from an open volume with a media fault. */
static pf_block_result_t ready(const pf_levelx_nor_t *volume)
{
    if (volume == NULL || !volume->opened) {
        return PF_BLOCK_NOT_READY;
    }
    return volume->faulted ? PF_BLOCK_IO_ERROR : PF_BLOCK_OK;
}

static pf_block_result_t geometry(void *context, pf_block_geometry_t *value)
{
    pf_levelx_nor_t *volume = context;
    pf_block_result_t result = ready(volume);
    if (result == PF_BLOCK_OK) {
        *value = (pf_block_geometry_t){
            .sector_count = volume->sector_count, .sector_size = 512u,
            .erase_sector_count = 1u, .write_protected = false,
        };
    }
    return result;
}

/** @brief Check the entire batch before dispatching any sector, including on direct callback use. */
static pf_block_result_t transfer_valid(pf_levelx_nor_t *volume, uint32_t first,
                                        const void *buffer, uint32_t count)
{
    pf_block_result_t result = ready(volume);
    if (result != PF_BLOCK_OK) {
        return result;
    }
    if (buffer == NULL || count == 0u || first >= volume->sector_count ||
        count > volume->sector_count - first || (uint64_t)count * 512u > SIZE_MAX) {
        return PF_BLOCK_INVALID_ARGUMENT;
    }
    return PF_BLOCK_OK;
}

/** @brief Expose only known-unmapped logical sectors as erased bytes; propagate all media faults. */
static pf_block_result_t read_blocks(void *context, uint32_t first, void *buffer, uint32_t count)
{
    pf_levelx_nor_t *volume = context;
    pf_block_result_t result = transfer_valid(volume, first, buffer, count);
    if (result != PF_BLOCK_OK) {
        return result;
    }
    uint8_t *bytes = buffer;
    for (uint32_t index = 0u; index < count; ++index) {
        uint8_t *sector = bytes + (size_t)index * 512u;
        UINT status = pf_levelx_nor_read(volume, first + index, sector);
        if (status == LX_SECTOR_NOT_FOUND && !volume->faulted) {
            memset(sector, 0xff, 512u);
        } else if (status != LX_SUCCESS) {
            return PF_BLOCK_IO_ERROR;
        }
    }
    return PF_BLOCK_OK;
}

static pf_block_result_t write_blocks(void *context, uint32_t first, const void *buffer, uint32_t count)
{
    pf_levelx_nor_t *volume = context;
    pf_block_result_t result = transfer_valid(volume, first, buffer, count);
    if (result != PF_BLOCK_OK) {
        return result;
    }
    const uint8_t *bytes = buffer;
    for (uint32_t index = 0u; index < count; ++index) {
        if (pf_levelx_nor_write(volume, first + index, bytes + (size_t)index * 512u) != LX_SUCCESS) {
            return PF_BLOCK_IO_ERROR;
        }
    }
    return PF_BLOCK_OK;
}

static pf_block_result_t sync_blocks(void *context)
{
    pf_levelx_nor_t *volume = context;
    pf_block_result_t result = ready(volume);
    if (result != PF_BLOCK_OK) {
        return result;
    }
    return pf_levelx_nor_sync(volume) == LX_SUCCESS ? PF_BLOCK_OK : PF_BLOCK_IO_ERROR;
}

pf_block_device_t pf_levelx_block_device(pf_levelx_nor_t *volume)
{
    static const pf_block_device_ops_t ops = {
        .geometry = geometry, .read = read_blocks, .write = write_blocks, .sync = sync_blocks,
    };
    return (pf_block_device_t){.ops = &ops, .context = volume};
}
