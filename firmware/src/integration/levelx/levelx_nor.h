#ifndef PF_LEVELX_NOR_H
#define PF_LEVELX_NOR_H

#include "drivers/nor_flash/nor.h"
#include "lx_api.h"

/** Caller-owned LevelX volume. Zero initialize before first use and serialize
 * all LevelX operations with the same external reservation. The application worker
 * and USB task may access it serially; ISR and second-core access are unsupported.
 * The raw flash and transport must outlive the volume. Do not copy an open volume.
 */
typedef struct {
    LX_NOR_FLASH control;
    pf_nor_t *flash;
    ULONG buffer[LX_NOR_SECTOR_SIZE];
    uint32_t offset;
    uint32_t size;
    uint32_t sector_count;
    pf_nor_result_t last_raw_result;
    UINT last_system_error;
    bool opened;
    bool faulted;
} pf_levelx_nor_t;

/** Open/recover an initialized NOR region, aligned to 4KiB and at least 4 blocks.
 * Returns LevelX status. Open may program/reclaim metadata, including on blank
 * media: never point this at an unrelated filesystem or valuable raw data.
 * No erase-all/format is performed. Reserve two erase blocks from exported capacity.
 */
UINT pf_levelx_nor_open(pf_levelx_nor_t *volume, pf_nor_t *flash,
                        uint32_t offset, uint32_t size);
/** Read/write one 512-byte sector; buffers need no special alignment.
 * Failed writes may have committed: inspect/recover, do not blindly replay.
 * After media errors, close, reinitialize raw transport/flash and reopen.
 */
UINT pf_levelx_nor_read(pf_levelx_nor_t *volume, uint32_t sector, void *data);
UINT pf_levelx_nor_write(pf_levelx_nor_t *volume, uint32_t sector, const void *data);
/** Synchronous raw-media completion, not FAT transaction atomicity. */
UINT pf_levelx_nor_sync(pf_levelx_nor_t *volume);
/** Close LevelX control state; does not release the caller's raw flash/transport. */
UINT pf_levelx_nor_close(pf_levelx_nor_t *volume);

#endif
