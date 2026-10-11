#include "sdspi_protocol.h"

#include <stddef.h>

#include "sd_card_block.h"

bool pf_sdspi_r1_is_ready_or_idle(uint8_t response)
{
    return (response == 0u) || (response == 1u);
}

bool pf_sdspi_read_token_is_valid(uint8_t token)
{
    return token == 0xfeu;
}

bool pf_sdspi_write_response_is_accepted(uint8_t response)
{
    return (response & 0x1fu) == 0x05u;
}

bool pf_sdspi_block_argument(uint32_t block, bool block_addressed,
                             uint32_t *argument)
{
    if (argument == NULL) {
        return false;
    }
    if (block_addressed) {
        *argument = block;
        return true;
    }
    if (block > (UINT32_MAX / PF_SD_CARD_BLOCK_SIZE)) {
        return false;
    }
    *argument = block * PF_SD_CARD_BLOCK_SIZE;
    return true;
}