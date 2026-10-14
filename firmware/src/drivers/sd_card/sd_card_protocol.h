#ifndef PF_SD_CARD_PROTOCOL_H
#define PF_SD_CARD_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sd_card_block.h"

/** Calculate the SD command CRC7; NULL input returns zero. */
uint8_t pf_sd_card_crc7(const uint8_t *data, size_t size);
/** Build a six-byte SD command in caller-owned storage, including CRC7. */
bool pf_sd_card_build_command(uint8_t command, uint32_t argument, uint8_t frame[6]);
/** Classify a native CMD8 argument; bit 22 indicates an illegal command. */
bool pf_sd_card_cmd8_supports_v2(uint32_t response_argument, bool *supports_v2);
/** Decode a caller-owned CSD v1/v2 register into a representable sector count. */
bool pf_sd_card_csd_sector_count(const uint8_t csd[16], uint32_t *sector_count);
/** Decode the CSD TRAN_SPEED field into hertz, rejecting reserved encodings. */
bool pf_sd_card_csd_transfer_hz(const uint8_t csd[16], uint32_t *transfer_hz);
/** Return whether the SCR advertises native four-bit bus support. */
bool pf_sd_card_scr_supports_four_bit(const uint8_t scr[8]);
/** Calculate serial SD data CRC16; NULL input returns zero. */
uint16_t pf_sd_card_crc16(const uint8_t *data, size_t size);

/** Driver-internal: run the idle hook, if any, while polling a long card wait. */
void pf_sd_card_idle(void);

/** Convert a sector index to an SDHC block or SDSC byte address, rejecting overflow. */
bool pf_sd_card_block_argument(uint32_t block, bool block_addressed, uint32_t *argument);

/** Validate buffer and capacity bounds; zero-length transfers are no-ops. */
pf_sd_card_result_t pf_sd_card_validate_transfer(uint32_t first_block, const void *buffer,
												uint32_t block_count, uint32_t capacity);

#endif