/** @file
 * @brief SD facade adapter for firmware-owned logical sector access.
 */
#ifndef PF_SD_BLOCK_DEVICE_H
#define PF_SD_BLOCK_DEVICE_H

#include "block_device.h"

/** Return a static descriptor; SD initialization and USB ownership transitions remain external.
 * Geometry remains queryable while USB owns initialized media; firmware read/write/sync still reject that owner.
 */
const pf_block_device_t *pf_storage_sd_block_device(void);

#endif
