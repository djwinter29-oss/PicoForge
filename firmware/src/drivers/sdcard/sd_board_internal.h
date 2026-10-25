/** @file
 * @brief Internal board binding implemented by the CMake-selected transport.
 */
#ifndef PF_SD_BOARD_INTERNAL_H
#define PF_SD_BOARD_INTERNAL_H

#include "sd_board.h"

/** Bind the persistent transport context on first use and return its borrowed link. */
pf_sd_link_t pf_sd_board_transport(bool bind);
/** Copy bus GPIOs into the prefilled status array and return the selected signaling mode. */
pf_sd_link_mode_t pf_sd_board_transport_pins(uint8_t bus_gpios[6]);
/** Return the selected socket's detect GPIO (UINT8_MAX for none) and write its polarity. */
uint8_t pf_sd_board_transport_detect(bool *active_high);

#endif
