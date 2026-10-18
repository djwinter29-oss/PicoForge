/** @file
 * @brief Build-selected MSC storage access; firmware descriptors never bypass ownership.
 */
#ifndef PF_MSC_STORAGE_H
#define PF_MSC_STORAGE_H

#include "middleware/storage/block_device.h"

#if PF_STORAGE_BACKEND_NOR
#include "app/storage/nor_media.h"

static inline void pf_msc_storage_lock(void) { pf_nor_media_lock(); }
static inline bool pf_msc_storage_try_lock(void) { return pf_nor_media_try_lock(); }
static inline void pf_msc_storage_unlock(void) { pf_nor_media_unlock(); }
static inline bool pf_msc_storage_ready(void) { return pf_nor_media_ready(); }
static inline uint32_t pf_msc_storage_count(void) { return pf_nor_media_sector_count(); }
static inline pf_block_result_t pf_msc_storage_acquire(void) { return pf_nor_media_acquire_usb(); }
static inline pf_block_result_t pf_msc_storage_release(void) { return pf_nor_media_release_usb(); }
static inline pf_block_result_t pf_msc_storage_release_for_recovery(void)
{
    return pf_nor_media_release_usb_for_recovery();
}
static inline pf_block_result_t pf_msc_storage_sync(void) { return pf_block_sync(pf_nor_media_usb_device()); }
static inline pf_block_result_t pf_msc_storage_read(uint32_t first, void *buffer, uint32_t count)
{
    return pf_block_read(pf_nor_media_usb_device(), first, buffer, count);
}
static inline pf_block_result_t pf_msc_storage_write(uint32_t first, const void *buffer, uint32_t count)
{
    return pf_block_write(pf_nor_media_usb_device(), first, buffer, count);
}
#else
#include "drivers/sd_card/sd_block.h"
#include "drivers/sd_card/sd_msc.h"

/** @brief Translate SD diagnostics without depending on result-enum numeric equivalence. */
static inline pf_block_result_t pf_msc_storage_result(pf_sd_result_t result)
{
    switch (result) {
    case PF_SD_OK: return PF_BLOCK_OK;
    case PF_SD_NOT_CONFIGURED:
    case PF_SD_NOT_READY: return PF_BLOCK_NOT_READY;
    case PF_SD_INVALID_ARGUMENT: return PF_BLOCK_INVALID_ARGUMENT;
    default: return PF_BLOCK_IO_ERROR;
    }
}
static inline void pf_msc_storage_lock(void) { pf_sd_lock(); }
static inline bool pf_msc_storage_try_lock(void) { return pf_sd_try_lock(); }
static inline void pf_msc_storage_unlock(void) { pf_sd_unlock(); }
static inline bool pf_msc_storage_ready(void) { return pf_sd_is_initialized(); }
static inline uint32_t pf_msc_storage_count(void) { return pf_sd_block_count(); }
static inline pf_block_result_t pf_msc_storage_acquire(void) { return pf_msc_storage_result(pf_sd_msc_acquire()); }
static inline pf_block_result_t pf_msc_storage_release(void) { return pf_msc_storage_result(pf_sd_msc_release()); }
static inline pf_block_result_t pf_msc_storage_release_for_recovery(void)
{
    return pf_msc_storage_result(pf_sd_msc_release());
}
static inline pf_block_result_t pf_msc_storage_sync(void) { return pf_msc_storage_result(pf_sd_msc_sync()); }
static inline pf_block_result_t pf_msc_storage_read(uint32_t first, void *buffer, uint32_t count)
{
    return pf_msc_storage_result(pf_sd_msc_read_blocks(first, buffer, count));
}
static inline pf_block_result_t pf_msc_storage_write(uint32_t first, const void *buffer, uint32_t count)
{
    return pf_msc_storage_result(pf_sd_msc_write_blocks(first, buffer, count));
}
#endif

#endif
