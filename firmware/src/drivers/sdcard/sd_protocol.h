#ifndef PF_SD_PROTOCOL_H
#define PF_SD_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sd.h"

/** Calculate the SD command CRC7; NULL input returns zero. */
uint8_t pf_sd_crc7(const uint8_t *data, size_t size);
/** Build a six-byte SD command in caller-owned storage, including CRC7. */
bool pf_sd_build_command(uint8_t command, uint32_t argument, uint8_t frame[6]);
/** Classify a native CMD8 argument; bit 22 indicates an illegal command. */
bool pf_sd_cmd8_supports_v2(uint32_t response_argument, bool *supports_v2);
/** Decode a caller-owned CSD v1/v2 register into a representable sector count. */
bool pf_sd_csd_sector_count(const uint8_t csd[16], uint32_t *sector_count);
/** Decode the CSD TRAN_SPEED field into hertz, rejecting reserved encodings. */
bool pf_sd_csd_transfer_hz(const uint8_t csd[16], uint32_t *transfer_hz);
/** Return whether the SCR advertises native four-bit bus support. */
bool pf_sd_scr_supports_four_bit(const uint8_t scr[8]);
/** Calculate serial SD data CRC16; NULL input returns zero. */
uint16_t pf_sd_crc16(const uint8_t *data, size_t size);

/** Driver-internal: run the idle hook, if any, while polling a long card wait. */
void pf_sd_idle(void);

/** Convert a sector index to an SDHC block or SDSC byte address, rejecting overflow. */
bool pf_sd_block_argument(uint32_t block, bool block_addressed, uint32_t *argument);

/** Validate buffer, capacity, and size_t byte bounds; zero-length transfers are no-ops. */
pf_sd_result_t pf_sd_validate_transfer(uint32_t first_block, const void *buffer,
                                      uint32_t block_count, uint32_t capacity);

/** Native R1 card-status error bits; not an R6 response mask. */
#define PF_SD_NATIVE_R1_ERROR_MASK 0xfdffe008u
/** Native R1 ready-for-data flag. */
#define PF_SD_NATIVE_R1_READY_FOR_DATA (1u << 8u)
/** Native R1 current-state field. */
#define PF_SD_NATIVE_R1_STATE_MASK (15u << 9u)
/** Native R1 transfer-state value. */
#define PF_SD_NATIVE_R1_STATE_TRAN (4u << 9u)
/** Native R6 response error bits. */
#define PF_SD_NATIVE_R6_ERROR_MASK 0xe000u
/** Native R1 illegal-command flag. */
#define PF_SD_NATIVE_R1_ILLEGAL_COMMAND (1u << 22u)
/** CMD8 voltage/check-pattern argument. */
#define PF_SD_NATIVE_R7_ARGUMENT 0x000001aau
/** CMD8 accepted-voltage field. */
#define PF_SD_NATIVE_R7_VOLTAGE_MASK 0x00000f00u
/** CMD8 accepted 2.7-3.6 V range. */
#define PF_SD_NATIVE_R7_VOLTAGE_3V3 0x00000100u
/** ACMD6 argument selecting four data lanes. */
#define PF_SD_NATIVE_BUS_WIDTH_FOUR_ARGUMENT 2u
/** ACMD41 operating-voltage window. */
#define PF_SD_NATIVE_OCR_VOLTAGE 0x00ff8000u
/** ACMD41 high-capacity support argument. */
#define PF_SD_NATIVE_OCR_HCS (1u << 30u)
/** OCR power-up-complete flag. */
#define PF_SD_NATIVE_OCR_READY (1u << 31u)
/** CMD6 argument checking high-speed support. */
#define PF_SD_NATIVE_CMD6_CHECK_HIGH_SPEED 0x00fffff1u
/** CMD6 argument selecting high-speed timing. */
#define PF_SD_NATIVE_CMD6_SWITCH_HIGH_SPEED 0x80fffff1u
/** CMD6 argument restoring default timing. */
#define PF_SD_NATIVE_CMD6_SWITCH_DEFAULT 0x80fffff0u
/** Words holding a 512-byte payload and four lane CRCs. */
#define PF_SD_NATIVE_TRANSFER_WORDS 130u
/** Bytes in a full-sector native DAT wire image. */
#define PF_SD_NATIVE_IMAGE_SIZE (PF_SD_NATIVE_TRANSFER_WORDS * 4u)
/** Bytes in a CMD6 switch-status payload. */
#define PF_SD_NATIVE_SWITCH_STATUS_SIZE 64u

#ifndef PF_SD_NATIVE_FUSED_COPY
/** Build-time selection of fused native CRC and payload copying. */
#define PF_SD_NATIVE_FUSED_COPY 0
#endif

/** Decode native short response framing/CRC, without interpreting card status. */
pf_sd_detail_error_t pf_sd_native_decode_response(
    uint8_t command, const uint8_t frame[6], uint32_t *argument, bool check_crc);
/** Decode a native R3 OCR response, which has no command index or CRC. */
bool pf_sd_native_parse_r3_response(const uint8_t frame[6], uint32_t *ocr);
/** Interpret the low native data-response bits as accepted or rejected. */
bool pf_sd_native_write_response_accepted(uint32_t response_word);
/** Validate native R2 response framing and register CRC. */
bool pf_sd_native_validate_r2_response(const uint8_t frame[17]);
/** Require a ready, error-free card in transfer state. */
bool pf_sd_native_status_is_transfer_ready(uint32_t status);
/** Check group-1 function 1 busy status in a version-1 switch-status block. */
bool pf_sd_native_switch_high_speed_busy(const uint8_t status[PF_SD_NATIVE_SWITCH_STATUS_SIZE]);
/** Calculate the four native lane CRC16s in DAT0-DAT3 order. */
void pf_sd_native_crc16_4bit(const uint8_t block[512], uint16_t crc[4]);

/**
 * Build the 520-byte DAT wire image: the ordered DAT0-DAT3 block bytes followed by
 * the four lane CRC16s. Byte order equals transmission order (high nibble first), so a
 * byte-swapping 32-bit DMA transfer feeds it to the PIO unchanged.
 */
bool pf_sd_native_encode_data_block(const uint8_t block[512], uint8_t image[PF_SD_NATIVE_IMAGE_SIZE]);
/** Encode a nonzero, word-sized payload up to 512 bytes followed by eight lane-CRC bytes. */
bool pf_sd_native_encode_data_block_sized(const uint8_t *block, size_t block_size,
                                         uint8_t image[PF_SD_NATIVE_IMAGE_SIZE]);
/** Restore a full-sector wire image and verify all lane CRCs; output is unspecified on failure. */
bool pf_sd_native_decode_data_block(const uint8_t image[PF_SD_NATIVE_IMAGE_SIZE], uint8_t block[512]);
/** Decode a nonzero, word-sized payload up to 512 bytes; output is unspecified on failure. */
bool pf_sd_native_decode_data_block_sized(const uint8_t *image, uint8_t *block, size_t block_size);
/** Verify a 520-byte sector wire image without retaining its payload. */
bool pf_sd_native_verify_data_block(const uint8_t image[PF_SD_NATIVE_IMAGE_SIZE]);
/** Read group-1 high-speed support from a CMD6 switch-status payload. */
bool pf_sd_native_switch_supports_high_speed(const uint8_t status[PF_SD_NATIVE_SWITCH_STATUS_SIZE]);
/** Read group-1 high-speed selection from a CMD6 switch-status payload. */
bool pf_sd_native_switch_selected_high_speed(const uint8_t status[PF_SD_NATIVE_SWITCH_STATUS_SIZE]);

#endif