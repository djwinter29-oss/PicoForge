#ifndef PF_SD_BOARD_H
#define PF_SD_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "sd.h"

/**
 * @brief Build the SD configuration from the board assignments and the active boot config.
 *
 * The first call binds the build-selected transport context and prepares the card-detect GPIO.
 * Both transports request the saved storage speed; card negotiation and hardware dividers determine the actual clock.
 * @param config Receives the configuration; its transport borrows driver-owned static state.
 * @return false only when @p config is NULL.
 */
bool pf_sd_board_config(pf_sd_config_t *config);

/**
 * @brief Report the build-selected bus and its board pins for status; UINT8_MAX means no assignment.
 * @param bus_gpios Receives SDIO CLK, CMD, D0-D3 or SPI SCK, MOSI, MISO, CS; unused entries are UINT8_MAX.
 * @param detect_gpio Receives the card-detect GPIO.
 * @return PF_SD_LINK_NATIVE_SD for SDIO-PIO or PF_SD_LINK_SPI for SD-SPI.
 */
pf_sd_link_mode_t pf_sd_board_pins(uint8_t bus_gpios[6], uint8_t *detect_gpio);

#endif
