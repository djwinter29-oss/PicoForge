#ifndef PF_SD_CARD_MSC_H
#define PF_SD_CARD_MSC_H

#include "sd_card_block.h"

/**
 * Give an initialized, unmounted card to USB MSC.
 *
 * The caller must serialize the complete lifecycle check and ensure FatFs is
 * unmounted before calling. Repeated acquisition is idempotent.
 */
pf_sd_card_result_t pf_sd_card_msc_acquire(void);

/**
 * Return the card to firmware after checked host eject/synchronization.
 *
 * The caller must serialize the complete lifecycle check. A real USB-to-firmware
 * transition requests card reinitialization before the next firmware mount.
 */
pf_sd_card_result_t pf_sd_card_msc_release(void);

/** Read blocks on behalf of the USB MSC host while it owns the card. */
pf_sd_card_result_t pf_sd_card_msc_read_blocks(uint32_t first_block, void *buffer,
                                               uint32_t block_count);

/** Write blocks on behalf of the USB MSC host while it owns the card. */
pf_sd_card_result_t pf_sd_card_msc_write_blocks(uint32_t first_block, const void *buffer,
                                                uint32_t block_count);

/** Flush writes on behalf of the USB MSC host while it owns the card. */
pf_sd_card_result_t pf_sd_card_msc_sync(void);

#endif
