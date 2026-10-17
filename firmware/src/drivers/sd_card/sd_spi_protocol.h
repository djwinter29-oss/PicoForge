#ifndef PF_SD_SPI_PROTOCOL_H
#define PF_SD_SPI_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

bool pf_sd_spi_r1_is_ready_or_idle(uint8_t response);
bool pf_sd_spi_read_token_is_valid(uint8_t token);
bool pf_sd_spi_write_response_is_pending(uint8_t response);
bool pf_sd_spi_write_response_is_accepted(uint8_t response);
bool pf_sd_spi_block_argument(uint32_t block, bool block_addressed,
                             uint32_t *argument);

#endif