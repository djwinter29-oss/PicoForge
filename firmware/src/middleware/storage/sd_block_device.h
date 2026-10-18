/** @file
 * @brief Raw SD protocol adapter; application ownership belongs to sd_media.
 */
#ifndef PF_SD_BLOCK_DEVICE_H
#define PF_SD_BLOCK_DEVICE_H

#include "block_device.h"

/** Return a static raw descriptor; callers serialize lifecycle and initialize the SD driver.
 * This adapter has no firmware/USB owner gate. Application FatFs binds sd_media's firmware descriptor instead.
 */
const pf_block_device_t *pf_sd_block_device(void);

#endif
