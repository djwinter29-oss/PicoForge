#ifndef PF_SD_CARD_SPI_PROTOCOL_H
#define PF_SD_CARD_SPI_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

bool pf_sd_spi_r1_is_ready_or_idle(uint8_t response);
bool pf_sd_spi_read_token_is_valid(uint8_t token);
bool pf_sd_spi_write_response_is_pending(uint8_t response);
bool pf_sd_spi_write_response_is_accepted(uint8_t response);
bool pf_sd_spi_block_argument(uint32_t block, bool block_addressed,
                             uint32_t *argument);

/** Return whether SCK, MOSI (TX) and MISO (RX) pins belong to SPI controller 0 or 1 (RP2040/RP2350). */
bool pf_sd_spi_pins_match_controller(uint32_t spi_index, uint32_t clock_gpio,
                                     uint32_t mosi_gpio, uint32_t miso_gpio);

#endif