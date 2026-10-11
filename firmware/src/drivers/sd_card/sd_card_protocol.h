#ifndef PF_SD_CARD_PROTOCOL_H
#define PF_SD_CARD_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint8_t pf_sd_card_crc7(const uint8_t *data, size_t size);
bool pf_sd_card_build_command(uint8_t command, uint32_t argument, uint8_t frame[6]);
bool pf_sd_card_cmd8_supports_v2(uint32_t response_argument, bool *supports_v2);
bool pf_sd_card_csd_sector_count(const uint8_t csd[16], uint32_t *sector_count);
uint16_t pf_sd_card_crc16(const uint8_t *data, size_t size);

#endif