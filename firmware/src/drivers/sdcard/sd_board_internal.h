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
/** Narrow a GPIO to its status encoding; out-of-range pins read as UINT8_MAX. */
static inline uint8_t pf_sd_board_pin_byte(uint32_t gpio)
{
    return gpio > UINT8_MAX - 1u ? UINT8_MAX : (uint8_t)gpio;
}

#endif
