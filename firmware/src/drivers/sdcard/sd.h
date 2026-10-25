#ifndef PF_SD_H
#define PF_SD_H

#include "sd_block.h"
#include "drivers/sdcard/link/sd_link.h"

/** @brief Protocol settings over a board-owned transport; its context outlives all SD access. */
typedef struct pf_sd_config {
    pf_sd_link_t transport; /**< Borrowed wire descriptor; mode must match the selected backend. */
    uint32_t clock_hz; /**< 0 selects 25 MHz; native accepts 0 or 25 MHz, SPI accepts 400 kHz to 50 MHz with CMD6. */
    bool native_default_speed_only; /**< Native only: skip the optional High-Speed (50 MHz) negotiation and stay at 25 MHz. */
} pf_sd_config_t;

/** @brief Initialize the build-selected backend; the first accepted configuration remains locked. */
pf_sd_result_t pf_sd_init(const pf_sd_config_t *config);
/** @brief Query the selected driver initialization stage for diagnostics. */
uint32_t pf_sd_get_init_stage(void);

#endif
