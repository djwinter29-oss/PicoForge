#ifndef PICOFORGE_SDIO_PROTOCOL_H
#define PICOFORGE_SDIO_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PICOFORGE_SDIO_TRANSFER_WORDS 130u

uint8_t picoforge_sdio_crc7(const uint8_t *data, size_t size);
bool picoforge_sdio_build_command(uint8_t command, uint32_t argument, uint8_t frame[6]);
bool picoforge_sdio_parse_response(uint8_t command, const uint8_t frame[6],
								   uint32_t *argument, bool check_crc);
bool picoforge_sdio_response_words_to_bytes(const uint32_t *words, uint16_t response_bits,
                                            uint8_t *response, size_t response_capacity);
bool picoforge_sdio_parse_r3_response(const uint8_t frame[6], uint32_t *ocr);
bool picoforge_sdio_write_response_accepted(uint32_t response_word);
bool picoforge_sdio_cmd8_supports_v2(uint32_t response_argument, bool *supports_v2);
bool picoforge_sdio_validate_r2_response(const uint8_t frame[17]);
bool picoforge_sdio_csd_sector_count(const uint8_t csd[16], uint32_t *sector_count);
void picoforge_sdio_crc16_4bit(const uint8_t block[512], uint16_t crc[4]);
void picoforge_sdio_pack_data_block(const uint8_t block[512], uint32_t words[PICOFORGE_SDIO_TRANSFER_WORDS]);
bool picoforge_sdio_unpack_data_block(const uint32_t words[PICOFORGE_SDIO_TRANSFER_WORDS],
									  uint8_t block[512]);

#endif