#include "sd_spi_protocol.h"

#include <stddef.h>

#include "sd_block.h"
#include "sd_protocol.h"

bool pf_sd_spi_r1_is_ready_or_idle(uint8_t response)
{
    return (response == 0u) || (response == 1u);
}

bool pf_sd_spi_read_token_is_valid(uint8_t token)
{
    return token == 0xfeu;
}

bool pf_sd_spi_write_response_is_pending(uint8_t response)
{
    return response == 0xffu;
}

bool pf_sd_spi_write_response_is_accepted(uint8_t response)
{
    return (response & 0x1fu) == 0x05u;
}

bool pf_sd_spi_block_argument(uint32_t block, bool block_addressed,
                             uint32_t *argument)
{
    return pf_sd_block_argument(block, block_addressed, argument);
}

static bool pin_has_role(uint32_t spi_index, uint32_t pin, uint32_t role)
{
    return ((pin % 4u) == role) && (((pin / 8u) % 2u) == spi_index);
}

bool pf_sd_spi_pins_match_controller(uint32_t spi_index, uint32_t clock_gpio,
                                     uint32_t mosi_gpio, uint32_t miso_gpio)
{
    /* Pin function pattern repeats per 4 GPIOs: RX, CSn, SCK, TX. */
    return (spi_index <= 1u) && pin_has_role(spi_index, miso_gpio, 0u) &&
           pin_has_role(spi_index, clock_gpio, 2u) && pin_has_role(spi_index, mosi_gpio, 3u);
}