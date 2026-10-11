#ifndef PF_SDSPI_PROTOCOL_H
#define PF_SDSPI_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

bool pf_sdspi_r1_is_ready_or_idle(uint8_t response);
bool pf_sdspi_read_token_is_valid(uint8_t token);
bool pf_sdspi_write_response_is_accepted(uint8_t response);
bool pf_sdspi_block_argument(uint32_t block, bool block_addressed,
                             uint32_t *argument);

#endif