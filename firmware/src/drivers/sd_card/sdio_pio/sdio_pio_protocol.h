#ifndef PF_SDIO_PIO_PROTOCOL_H
#define PF_SDIO_PIO_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PF_SDIO_PIO_TRANSFER_WORDS 130u
#define PF_SDIO_PIO_IMAGE_SIZE (PF_SDIO_PIO_TRANSFER_WORDS * 4u)
#define PF_SDIO_PIO_SWITCH_STATUS_SIZE 64u

uint8_t pf_sdio_pio_crc7(const uint8_t *data, size_t size);
bool pf_sdio_pio_build_command(uint8_t command, uint32_t argument, uint8_t frame[6]);
bool pf_sdio_pio_parse_response(uint8_t command, const uint8_t frame[6],
								   uint32_t *argument, bool check_crc);
bool pf_sdio_pio_response_words_to_bytes(const uint32_t *words, uint16_t response_bits,
                                            uint8_t *response, size_t response_capacity);
bool pf_sdio_pio_parse_r3_response(const uint8_t frame[6], uint32_t *ocr);
bool pf_sdio_pio_write_response_accepted(uint32_t response_word);
bool pf_sdio_pio_cmd8_supports_v2(uint32_t response_argument, bool *supports_v2);
bool pf_sdio_pio_validate_r2_response(const uint8_t frame[17]);
bool pf_sdio_pio_csd_sector_count(const uint8_t csd[16], uint32_t *sector_count);
void pf_sdio_pio_crc16_4bit(const uint8_t block[512], uint16_t crc[4]);

/**
 * Build the 520-byte DAT wire image: the ordered DAT0-DAT3 block bytes followed by
 * the four lane CRC16s. Byte order equals transmission order (high nibble first), so a
 * byte-swapping 32-bit DMA transfer feeds it to the PIO unchanged.
 */
bool pf_sdio_pio_encode_data_block(const uint8_t block[512],
                                   uint8_t image[PF_SDIO_PIO_IMAGE_SIZE]);
bool pf_sdio_pio_encode_data_block_sized(const uint8_t *block, size_t block_size,
                                         uint8_t image[PF_SDIO_PIO_IMAGE_SIZE]);

/**
 * Restore a received wire image to a logical block and verify all four lane CRCs.
 * The block contents are unspecified when this returns false.
 */
bool pf_sdio_pio_decode_data_block(const uint8_t image[PF_SDIO_PIO_IMAGE_SIZE],
                                   uint8_t block[512]);
bool pf_sdio_pio_decode_data_block_sized(const uint8_t *image, uint8_t *block,
                                         size_t block_size);
bool pf_sdio_pio_switch_supports_high_speed(
    const uint8_t status[PF_SDIO_PIO_SWITCH_STATUS_SIZE]);
bool pf_sdio_pio_switch_selected_high_speed(
    const uint8_t status[PF_SDIO_PIO_SWITCH_STATUS_SIZE]);

#endif