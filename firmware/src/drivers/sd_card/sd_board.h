#ifndef PF_SD_BOARD_H
#define PF_SD_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "sd.h"

/** @brief Board-supplied SD-card assignment. */
typedef struct {
    uint32_t clock_hz; /**< Transport ceiling; above 25 MHz requires card High-Speed support. */
    uint32_t card_detect_gpio; /**< Card-detect GPIO. */
    bool card_detect_active_high; /**< Card-detect polarity. */
} pf_sd_board_t;

/** Build-selected assignment, defined in config/board.c. */
extern const pf_sd_board_t pf_sd_board;

/**
 * @brief Build the SD configuration from the board assignments and the active boot config.
 *
 * The first call binds the build-selected transport context and prepares the card-detect GPIO.
 * @param config Receives the configuration; its transport borrows driver-owned static state.
 * @return false only when @p config is NULL.
 */
bool pf_sd_board_config(pf_sd_config_t *config);

#endif
