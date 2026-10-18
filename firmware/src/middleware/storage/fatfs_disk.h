/** @file
 * @brief Middleware binding of a logical block device to FatFs drive zero.
 */
#ifndef PF_FATFS_DISK_H
#define PF_FATFS_DISK_H

#include "block_device.h"

/** Bind a borrowed descriptor before mounting, or NULL after unmounting.
 * Serialize with all FatFs calls; never replace the device while files are open or the volume is mounted.
 * Descriptor, context and operations must remain alive until unbound. This does not initialize media.
 */
void pf_fatfs_disk_bind(const pf_block_device_t *device);

#endif
