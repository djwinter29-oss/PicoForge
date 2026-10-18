/** @file
 * @brief Logical block dispatch and geometry/range validation; no additional locks or retries.
 */
#include "block_device.h"

#include <stddef.h>

static bool valid_device(const pf_block_device_t *device)
{
    return device != NULL && device->ops != NULL && device->ops->geometry != NULL &&
           device->ops->read != NULL && device->ops->write != NULL && device->ops->sync != NULL;
}

pf_block_result_t pf_block_geometry(const pf_block_device_t *device, pf_block_geometry_t *geometry)
{
    if (!valid_device(device) || geometry == NULL) {
        return PF_BLOCK_INVALID_ARGUMENT;
    }
    pf_block_result_t result = device->ops->geometry(device->context, geometry);
    if (result == PF_BLOCK_OK &&
        (geometry->sector_count == 0u || geometry->sector_size == 0u || geometry->erase_sector_count == 0u)) {
        return PF_BLOCK_IO_ERROR;
    }
    return result;
}

/** @brief Validate a batch without overflow; backend must recheck media/access state during I/O. */
static pf_block_result_t validate_transfer(const pf_block_device_t *device, uint32_t first,
                                          const void *buffer, uint32_t count, bool write)
{
    if (buffer == NULL || count == 0u) {
        return PF_BLOCK_INVALID_ARGUMENT;
    }
    pf_block_geometry_t geometry;
    pf_block_result_t result = pf_block_geometry(device, &geometry);
    if (result != PF_BLOCK_OK) {
        return result;
    }
    if (first >= geometry.sector_count || count > geometry.sector_count - first ||
        (uint64_t)count * geometry.sector_size > SIZE_MAX) {
        return PF_BLOCK_INVALID_ARGUMENT;
    }
    return write && geometry.write_protected ? PF_BLOCK_WRITE_PROTECTED : PF_BLOCK_OK;
}

pf_block_result_t pf_block_read(const pf_block_device_t *device, uint32_t first, void *buffer, uint32_t count)
{
    pf_block_result_t result = validate_transfer(device, first, buffer, count, false);
    return result == PF_BLOCK_OK ? device->ops->read(device->context, first, buffer, count) : result;
}

pf_block_result_t pf_block_write(const pf_block_device_t *device, uint32_t first, const void *buffer, uint32_t count)
{
    pf_block_result_t result = validate_transfer(device, first, buffer, count, true);
    return result == PF_BLOCK_OK ? device->ops->write(device->context, first, buffer, count) : result;
}

pf_block_result_t pf_block_sync(const pf_block_device_t *device)
{
    return valid_device(device) ? device->ops->sync(device->context) : PF_BLOCK_INVALID_ARGUMENT;
}
