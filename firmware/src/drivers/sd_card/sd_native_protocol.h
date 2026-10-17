#ifndef PF_SD_NATIVE_PROTOCOL_H
#define PF_SD_NATIVE_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sd_block.h"

/* Native R1 card-status error bits; not an R6 response mask. */
#define PF_SD_NATIVE_R1_ERROR_MASK 0xfdffe008u
#define PF_SD_NATIVE_R1_READY_FOR_DATA (1u << 8u)
#define PF_SD_NATIVE_R1_STATE_MASK (15u << 9u)
#define PF_SD_NATIVE_R1_STATE_TRAN (4u << 9u)
#define PF_SD_NATIVE_R6_ERROR_MASK 0xe000u
#define PF_SD_NATIVE_R1_ILLEGAL_COMMAND (1u << 22u)
#define PF_SD_NATIVE_R7_ARGUMENT 0x000001aau
#define PF_SD_NATIVE_R7_VOLTAGE_MASK 0x00000f00u
#define PF_SD_NATIVE_R7_VOLTAGE_3V3 0x00000100u
#define PF_SD_NATIVE_BUS_WIDTH_FOUR_ARGUMENT 2u
#define PF_SD_NATIVE_OCR_VOLTAGE 0x00ff8000u
#define PF_SD_NATIVE_OCR_HCS (1u << 30u)
#define PF_SD_NATIVE_OCR_READY (1u << 31u)
#define PF_SD_NATIVE_CMD6_CHECK_HIGH_SPEED 0x00fffff1u
#define PF_SD_NATIVE_CMD6_SWITCH_HIGH_SPEED 0x80fffff1u
#define PF_SD_NATIVE_CMD6_SWITCH_DEFAULT 0x80fffff0u

#define PF_SD_NATIVE_TRANSFER_WORDS 130u
#define PF_SD_NATIVE_IMAGE_SIZE (PF_SD_NATIVE_TRANSFER_WORDS * 4u)
#define PF_SD_NATIVE_SWITCH_STATUS_SIZE 64u

#ifndef PF_SD_NATIVE_FUSED_COPY
#define PF_SD_NATIVE_FUSED_COPY 0
#endif

/** Decode native short response framing/CRC, without interpreting card status. */
pf_sd_detail_error_t pf_sd_native_decode_response(
    uint8_t command, const uint8_t frame[6], uint32_t *argument, bool check_crc);
bool pf_sd_native_response_words_to_bytes(const uint32_t *words, uint16_t response_bits,
                                            uint8_t *response, size_t response_capacity);
bool pf_sd_native_parse_r3_response(const uint8_t frame[6], uint32_t *ocr);
bool pf_sd_native_write_response_accepted(uint32_t response_word);
bool pf_sd_native_validate_r2_response(const uint8_t frame[17]);
/** Require a ready, error-free card in transfer state. */
bool pf_sd_native_status_is_transfer_ready(uint32_t status);
/** Check group-1 function 1 busy status in a version-1 switch-status block. */
bool pf_sd_native_switch_high_speed_busy(
    const uint8_t status[PF_SD_NATIVE_SWITCH_STATUS_SIZE]);
void pf_sd_native_crc16_4bit(const uint8_t block[512], uint16_t crc[4]);

/**
 * Build the 520-byte DAT wire image: the ordered DAT0-DAT3 block bytes followed by
 * the four lane CRC16s. Byte order equals transmission order (high nibble first), so a
 * byte-swapping 32-bit DMA transfer feeds it to the PIO unchanged.
 */
bool pf_sd_native_encode_data_block(const uint8_t block[512],
                                   uint8_t image[PF_SD_NATIVE_IMAGE_SIZE]);
bool pf_sd_native_encode_data_block_sized(const uint8_t *block, size_t block_size,
                                         uint8_t image[PF_SD_NATIVE_IMAGE_SIZE]);

/**
 * Restore a received wire image to a logical block and verify all four lane CRCs.
 * The block contents are unspecified when this returns false.
 */
bool pf_sd_native_decode_data_block(const uint8_t image[PF_SD_NATIVE_IMAGE_SIZE],
                                   uint8_t block[512]);
bool pf_sd_native_decode_data_block_sized(const uint8_t *image, uint8_t *block,
                                         size_t block_size);
/** Verify a 520-byte sector wire image without retaining its payload. */
bool pf_sd_native_verify_data_block(const uint8_t image[PF_SD_NATIVE_IMAGE_SIZE]);
bool pf_sd_native_switch_supports_high_speed(
    const uint8_t status[PF_SD_NATIVE_SWITCH_STATUS_SIZE]);
bool pf_sd_native_switch_selected_high_speed(
    const uint8_t status[PF_SD_NATIVE_SWITCH_STATUS_SIZE]);

#endif