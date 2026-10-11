#include "sd_spi_protocol.h"

#include <stddef.h>

#include "sd_card_block.h"
#include "sd_card_protocol.h"

bool pf_sd_spi_r1_is_ready_or_idle(uint8_t response)
{
    return (response == 0u) || (response == 1u);
}

bool pf_sd_spi_read_token_is_valid(uint8_t token)
{
    return token == 0xfeu;
}

bool pf_sd_spi_write_response_is_accepted(uint8_t response)
{
    return (response & 0x1fu) == 0x05u;
}

bool pf_sd_spi_block_argument(uint32_t block, bool block_addressed,
                             uint32_t *argument)
{
    return pf_sd_card_block_argument(block, block_addressed, argument);
}