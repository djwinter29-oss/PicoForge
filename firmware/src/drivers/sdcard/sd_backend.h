/** @file
 * @brief Internal protocol contract implemented by exactly one CMake-selected SD backend.
 *
 * These operations do not acquire the execution-port reservation. Use the public sd.h facade,
 * which serializes calls and rejects unconfigured access; direct backend tests must provide serialization.
 * Once locked, configuration persists across initialization failures and removal. Media readiness is separate.
 */
#ifndef PF_SD_BACKEND_H
#define PF_SD_BACKEND_H

#include "sd.h"

/** Return the signaling mode accepted by the selected implementation. */
pf_sd_link_mode_t pf_sd_backend_mode(void);
/** Initialize or retry the singleton.
 * Native SD locks valid configuration before physical setup; SPI locks after physical setup succeeds.
 * Locked configuration survives identification failure and removal; different settings are rejected.
 */
pf_sd_result_t pf_sd_backend_init(const pf_sd_config_t *config);
/** Return whether initialized media remains present. */
bool pf_sd_backend_is_initialized(void);
/** Return cached capacity in 512-byte sectors; the caller must establish media readiness first. */
uint32_t pf_sd_backend_block_count(void);
/** Read sectors without replay; the buffer may be partially updated on failure. */
pf_sd_result_t pf_sd_backend_read_blocks(uint32_t first_block, void *buffer, uint32_t block_count);
/** Write sectors without replay; some sectors may have committed on failure. */
pf_sd_result_t pf_sd_backend_write_blocks(uint32_t first_block, const void *buffer, uint32_t block_count);
/** Complete pending writes and check device completion. */
pf_sd_result_t pf_sd_backend_sync(void);
/** Copy the selected backend's retained media and error information. */
void pf_sd_backend_get_info(pf_sd_info_t *info);
/** Confirm debounced removal without sending card commands; SPI requires no additional cleanup. */
void pf_sd_backend_confirm_removed(void);

#endif
