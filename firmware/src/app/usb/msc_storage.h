/** @file
 * @brief SD MSC storage access; firmware descriptors never bypass ownership.
 */
#ifndef PF_MSC_STORAGE_H
#define PF_MSC_STORAGE_H

#include "middleware/storage/block_device.h"

#include "drivers/sdcard/sd_block.h"
#include "app/storage/sd_media.h"

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
static inline pf_block_result_t pf_msc_storage_acquire(void) { return pf_msc_storage_result(pf_sd_media_acquire_usb()); }
static inline pf_block_result_t pf_msc_storage_release(void) { return pf_msc_storage_result(pf_sd_media_release_usb()); }
static inline pf_block_result_t pf_msc_storage_release_for_recovery(void)
{
    return pf_msc_storage_result(pf_sd_media_release_usb_for_recovery());
}
static inline pf_block_result_t pf_msc_storage_sync(void) { return pf_msc_storage_result(pf_sd_media_usb_sync()); }
static inline pf_block_result_t pf_msc_storage_read(uint32_t first, void *buffer, uint32_t count)
{
    return pf_msc_storage_result(pf_sd_media_usb_read_blocks(first, buffer, count));
}
static inline pf_block_result_t pf_msc_storage_write(uint32_t first, const void *buffer, uint32_t count)
{
    return pf_msc_storage_result(pf_sd_media_usb_write_blocks(first, buffer, count));
}
#endif
