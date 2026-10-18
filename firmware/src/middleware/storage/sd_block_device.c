/** @file
 * @brief Adapt raw serialized SD sectors; application access must use the sd_media owner-gated descriptor.
 */
#include "sd_block_device.h"
#include "sd_block.h"

#include <stddef.h>

static pf_block_result_t sd_result_to_block(pf_sd_result_t result)
{
    switch (result) {
    case PF_SD_OK: return PF_BLOCK_OK;
    case PF_SD_INVALID_ARGUMENT: return PF_BLOCK_INVALID_ARGUMENT;
    case PF_SD_NOT_READY:
    case PF_SD_NOT_CONFIGURED: return PF_BLOCK_NOT_READY;
    default: return PF_BLOCK_IO_ERROR;
    }
}

/** @brief Snapshot raw metadata under the driver reservation; no application ownership is implied. */
static pf_block_result_t geometry(void *context, pf_block_geometry_t *geometry)
{
    (void)context;
    pf_sd_lock();
    pf_block_result_t result = PF_BLOCK_NOT_READY;
    if (pf_sd_is_initialized()) {
        *geometry = (pf_block_geometry_t){
            .sector_count = pf_sd_block_count(),
            .sector_size = PF_SD_BLOCK_SIZE,
            .erase_sector_count = 1u,
            .write_protected = false,
        };
        result = PF_BLOCK_OK;
    }
    pf_sd_unlock();
    return result;
}

static pf_block_result_t read_blocks(void *context, uint32_t first, void *buffer, uint32_t count)
{
    (void)context;
    return sd_result_to_block(pf_sd_read_blocks(first, buffer, count));
}

static pf_block_result_t write_blocks(void *context, uint32_t first, const void *buffer, uint32_t count)
{
    (void)context;
    return sd_result_to_block(pf_sd_write_blocks(first, buffer, count));
}

static pf_block_result_t sync_blocks(void *context)
{
    (void)context;
    return sd_result_to_block(pf_sd_sync());
}

const pf_block_device_t *pf_sd_block_device(void)
{
    static const pf_block_device_ops_t ops = {
        .geometry = geometry, .read = read_blocks, .write = write_blocks, .sync = sync_blocks,
    };
    static const pf_block_device_t device = {.ops = &ops, .context = NULL};
    return &device;
}
