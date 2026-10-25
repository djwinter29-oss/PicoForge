#ifndef PF_SD_H
#define PF_SD_H

#include "sd_block.h"
#include "drivers/sdcard/sd_link.h"

/** @brief Protocol settings over a board-owned transport; its context outlives all SD access. */
typedef struct pf_sd_config {
    pf_sd_link_t transport; /**< Borrowed wire descriptor; mode must match the selected backend. */
    uint32_t clock_hz; /**< Requested target; rounded down. 0: 25 MHz; native: 25/50 MHz; SPI: 400 kHz-50 MHz. */
    bool native_default_speed_only; /**< Native only: disallow High-Speed negotiation even for a 50 MHz request. */
} pf_sd_config_t;

/** @brief Initialize the build-selected backend; the first accepted configuration remains locked. */
pf_sd_result_t pf_sd_init(const pf_sd_config_t *config);

#endif
