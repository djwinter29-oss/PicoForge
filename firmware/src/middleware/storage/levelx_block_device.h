/** @file
 * @brief Logical block-device descriptor for an already-open LevelX NOR volume.
 */
#ifndef PF_LEVELX_BLOCK_DEVICE_H
#define PF_LEVELX_BLOCK_DEVICE_H

#include "block_device.h"
#include "integration/levelx/levelx_nor.h"

/** @brief Build a borrowed descriptor without opening, formatting, or selecting flash.
 * @param volume Caller-owned, initialized LevelX control state; must outlive the descriptor.
 * @return Descriptor by value. Caller serializes all operations and ownership transitions.
 */
pf_block_device_t pf_levelx_block_device(pf_levelx_nor_t *volume);

#endif
