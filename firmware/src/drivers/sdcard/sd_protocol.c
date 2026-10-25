/** @file SD command/register helpers, native response decoding and four-lane data codecs. */
#include "sd_protocol.h"

#include <string.h>

/* CRC-16/XMODEM (poly 0x1021, init 0), the SD data-block CRC. */
static const uint16_t crc16_table[256] = {
    0x0000u, 0x1021u, 0x2042u, 0x3063u, 0x4084u, 0x50a5u, 0x60c6u, 0x70e7u,
    0x8108u, 0x9129u, 0xa14au, 0xb16bu, 0xc18cu, 0xd1adu, 0xe1ceu, 0xf1efu,
    0x1231u, 0x0210u, 0x3273u, 0x2252u, 0x52b5u, 0x4294u, 0x72f7u, 0x62d6u,
    0x9339u, 0x8318u, 0xb37bu, 0xa35au, 0xd3bdu, 0xc39cu, 0xf3ffu, 0xe3deu,
    0x2462u, 0x3443u, 0x0420u, 0x1401u, 0x64e6u, 0x74c7u, 0x44a4u, 0x5485u,
    0xa56au, 0xb54bu, 0x8528u, 0x9509u, 0xe5eeu, 0xf5cfu, 0xc5acu, 0xd58du,
    0x3653u, 0x2672u, 0x1611u, 0x0630u, 0x76d7u, 0x66f6u, 0x5695u, 0x46b4u,
    0xb75bu, 0xa77au, 0x9719u, 0x8738u, 0xf7dfu, 0xe7feu, 0xd79du, 0xc7bcu,
    0x48c4u, 0x58e5u, 0x6886u, 0x78a7u, 0x0840u, 0x1861u, 0x2802u, 0x3823u,
    0xc9ccu, 0xd9edu, 0xe98eu, 0xf9afu, 0x8948u, 0x9969u, 0xa90au, 0xb92bu,
    0x5af5u, 0x4ad4u, 0x7ab7u, 0x6a96u, 0x1a71u, 0x0a50u, 0x3a33u, 0x2a12u,
    0xdbfdu, 0xcbdcu, 0xfbbfu, 0xeb9eu, 0x9b79u, 0x8b58u, 0xbb3bu, 0xab1au,
    0x6ca6u, 0x7c87u, 0x4ce4u, 0x5cc5u, 0x2c22u, 0x3c03u, 0x0c60u, 0x1c41u,
    0xedaeu, 0xfd8fu, 0xcdecu, 0xddcdu, 0xad2au, 0xbd0bu, 0x8d68u, 0x9d49u,
    0x7e97u, 0x6eb6u, 0x5ed5u, 0x4ef4u, 0x3e13u, 0x2e32u, 0x1e51u, 0x0e70u,
    0xff9fu, 0xefbeu, 0xdfddu, 0xcffcu, 0xbf1bu, 0xaf3au, 0x9f59u, 0x8f78u,
    0x9188u, 0x81a9u, 0xb1cau, 0xa1ebu, 0xd10cu, 0xc12du, 0xf14eu, 0xe16fu,
    0x1080u, 0x00a1u, 0x30c2u, 0x20e3u, 0x5004u, 0x4025u, 0x7046u, 0x6067u,
    0x83b9u, 0x9398u, 0xa3fbu, 0xb3dau, 0xc33du, 0xd31cu, 0xe37fu, 0xf35eu,
    0x02b1u, 0x1290u, 0x22f3u, 0x32d2u, 0x4235u, 0x5214u, 0x6277u, 0x7256u,
    0xb5eau, 0xa5cbu, 0x95a8u, 0x8589u, 0xf56eu, 0xe54fu, 0xd52cu, 0xc50du,
    0x34e2u, 0x24c3u, 0x14a0u, 0x0481u, 0x7466u, 0x6447u, 0x5424u, 0x4405u,
    0xa7dbu, 0xb7fau, 0x8799u, 0x97b8u, 0xe75fu, 0xf77eu, 0xc71du, 0xd73cu,
    0x26d3u, 0x36f2u, 0x0691u, 0x16b0u, 0x6657u, 0x7676u, 0x4615u, 0x5634u,
    0xd94cu, 0xc96du, 0xf90eu, 0xe92fu, 0x99c8u, 0x89e9u, 0xb98au, 0xa9abu,
    0x5844u, 0x4865u, 0x7806u, 0x6827u, 0x18c0u, 0x08e1u, 0x3882u, 0x28a3u,
    0xcb7du, 0xdb5cu, 0xeb3fu, 0xfb1eu, 0x8bf9u, 0x9bd8u, 0xabbbu, 0xbb9au,
    0x4a75u, 0x5a54u, 0x6a37u, 0x7a16u, 0x0af1u, 0x1ad0u, 0x2ab3u, 0x3a92u,
    0xfd2eu, 0xed0fu, 0xdd6cu, 0xcd4du, 0xbdaau, 0xad8bu, 0x9de8u, 0x8dc9u,
    0x7c26u, 0x6c07u, 0x5c64u, 0x4c45u, 0x3ca2u, 0x2c83u, 0x1ce0u, 0x0cc1u,
    0xef1fu, 0xff3eu, 0xcf5du, 0xdf7cu, 0xaf9bu, 0xbfbau, 0x8fd9u, 0x9ff8u,
    0x6e17u, 0x7e36u, 0x4e55u, 0x5e74u, 0x2e93u, 0x3eb2u, 0x0ed1u, 0x1ef0u,
};

static void (*idle_hook)(void); /**< Borrowed pre-transfer hook shared by both protocols' polling waits. */

/* Public configuration lives beside its polling implementation; no facade forwarding layer is needed. */
void pf_sd_set_idle_hook(void (*hook)(void))
{
    idle_hook = hook;
}

void pf_sd_idle(void)
{
    if (idle_hook != NULL) {
        idle_hook();
    }
}

bool pf_sd_block_argument(uint32_t block, bool block_addressed, uint32_t *argument)
{
    if ((argument == NULL) ||
        (!block_addressed && (block > UINT32_MAX / PF_SD_BLOCK_SIZE))) {
        return false;
    }
    *argument = block_addressed ? block : block * PF_SD_BLOCK_SIZE;
    return true;
}

pf_sd_result_t pf_sd_validate_transfer(uint32_t first_block, const void *buffer,
                                                uint32_t block_count, uint32_t capacity)
{
    if (block_count == 0u) {
        return PF_SD_OK;
    }
    if ((buffer == NULL) || (first_block >= capacity) ||
        (block_count > capacity - first_block) ||
        ((uint64_t)block_count * PF_SD_BLOCK_SIZE > SIZE_MAX)) {
        return PF_SD_INVALID_ARGUMENT;
    }
    return PF_SD_OK;
}

uint8_t pf_sd_crc7(const uint8_t *data, size_t size)
{
    uint8_t crc = 0u;

    if (data == NULL) {
        return 0u;
    }

    for (size_t byte_index = 0u; byte_index < size; byte_index++) {
        uint8_t value = data[byte_index];
        for (uint8_t bit_index = 0u; bit_index < 8u; bit_index++) {
            bool feedback = ((crc & 0x40u) != 0u) ^ ((value & 0x80u) != 0u);
            crc = (uint8_t)((crc << 1u) & 0x7fu);
            if (feedback) {
                crc ^= 0x09u;
            }
            value <<= 1u;
        }
    }
    return crc;
}

bool pf_sd_build_command(uint8_t command, uint32_t argument, uint8_t frame[6])
{
    if ((command > 63u) || (frame == NULL)) {
        return false;
    }
    frame[0] = (uint8_t)(0x40u | command);
    frame[1] = (uint8_t)(argument >> 24u);
    frame[2] = (uint8_t)(argument >> 16u);
    frame[3] = (uint8_t)(argument >> 8u);
    frame[4] = (uint8_t)argument;
    frame[5] = (uint8_t)((pf_sd_crc7(frame, 5u) << 1u) | 1u);
    return true;
}

bool pf_sd_cmd8_supports_v2(uint32_t response_argument, bool *supports_v2)
{
    const uint32_t illegal_command = 1u << 22u;

    if (supports_v2 == NULL) {
        return false;
    }
    if ((response_argument & illegal_command) != 0u) {
        *supports_v2 = false;
        return true;
    }
    if ((response_argument & 0xfffu) != 0x1aau) {
        return false;
    }
    *supports_v2 = true;
    return true;
}

bool pf_sd_csd_sector_count(const uint8_t csd[16], uint32_t *sector_count)
{
    uint64_t sectors;

    if ((csd == NULL) || (sector_count == NULL)) {
        return false;
    }

    if ((csd[0] >> 6u) == 1u) {
        uint32_t c_size = ((uint32_t)(csd[7] & 0x3fu) << 16u) |
                          ((uint32_t)csd[8] << 8u) | (uint32_t)csd[9];
        sectors = ((uint64_t)c_size + 1u) * 1024u;
    } else if ((csd[0] >> 6u) == 0u) {
        uint32_t read_bl_len = csd[5] & 0x0fu;
        uint32_t c_size = ((uint32_t)(csd[6] & 0x03u) << 10u) |
                          ((uint32_t)csd[7] << 2u) | ((uint32_t)csd[8] >> 6u);
        uint32_t c_size_mult = ((uint32_t)(csd[9] & 0x03u) << 1u) |
                               ((uint32_t)csd[10] >> 7u);
        if (read_bl_len > 11u) {
            return false;
        }
        sectors = ((uint64_t)c_size + 1u) * (1u << (c_size_mult + 2u)) *
                  (1u << read_bl_len) / PF_SD_BLOCK_SIZE;
    } else {
        return false;
    }

    if ((sectors == 0u) || (sectors > UINT32_MAX)) {
        return false;
    }
    *sector_count = (uint32_t)sectors;
    return true;
}

bool pf_sd_csd_transfer_hz(const uint8_t csd[16], uint32_t *transfer_hz)
{
    static const uint8_t multiplier_tenths[16] = {
        0u, 10u, 12u, 13u, 15u, 20u, 25u, 30u,
        35u, 40u, 45u, 50u, 55u, 60u, 70u, 80u,
    };
    static const uint32_t unit_hz[4] = {
        100000u, 1000000u, 10000000u, 100000000u,
    };
    uint8_t tran_speed;
    uint8_t multiplier;
    uint8_t unit;

    if ((csd == NULL) || (transfer_hz == NULL) || ((csd[3] & 0x80u) != 0u)) {
        return false;
    }
    tran_speed = csd[3];
    multiplier = (tran_speed >> 3u) & 0x0fu;
    unit = tran_speed & 0x07u;
    if ((multiplier == 0u) || (unit >= (sizeof(unit_hz) / sizeof(unit_hz[0])))) {
        return false;
    }
    *transfer_hz = (uint32_t)(((uint64_t)multiplier_tenths[multiplier] * unit_hz[unit]) / 10u);
    return *transfer_hz != 0u;
}

bool pf_sd_scr_supports_four_bit(const uint8_t scr[8])
{
    return (scr != NULL) && ((scr[1] & 0x04u) != 0u);
}

uint16_t pf_sd_crc16(const uint8_t *data, size_t size)
{
    uint16_t crc = 0u;

    if (data == NULL) {
        return 0u;
    }
    for (size_t byte_index = 0u; byte_index < size; byte_index++) {
        crc = (uint16_t)((crc << 8u) ^ crc16_table[(crc >> 8u) ^ data[byte_index]]);
    }
    return crc;
}

pf_sd_detail_error_t pf_sd_native_decode_response(
    uint8_t command, const uint8_t frame[6], uint32_t *argument, bool check_crc)
{
    if (frame == NULL || argument == NULL) {
        return PF_SD_DETAIL_HOST_STATE;
    }
    if ((frame[0] & 0xc0u) != 0u || (frame[0] & 0x3fu) != command) {
        return PF_SD_DETAIL_RESPONSE_INDEX;
    }
    if ((frame[5] & 1u) == 0u ||
        (check_crc && ((frame[5] >> 1u) != pf_sd_crc7(frame, 5u)))) {
        return PF_SD_DETAIL_RESPONSE_CRC;
    }

    *argument = ((uint32_t)frame[1] << 24u) |
                ((uint32_t)frame[2] << 16u) |
                ((uint32_t)frame[3] << 8u) |
                (uint32_t)frame[4];
    return PF_SD_DETAIL_NONE;
}

bool pf_sd_native_parse_r3_response(const uint8_t frame[6], uint32_t *ocr)
{
    if ((frame == NULL) || (ocr == NULL) ||
        (frame[0] != 0x3fu) || (frame[5] != 0xffu)) {
        return false;
    }

    *ocr = ((uint32_t)frame[1] << 24u) |
           ((uint32_t)frame[2] << 16u) |
           ((uint32_t)frame[3] << 8u) |
           (uint32_t)frame[4];
    return true;
}

bool pf_sd_native_write_response_accepted(uint32_t response_word)
{
    return (response_word & 0x0fu) == 0x05u;
}

bool pf_sd_native_validate_r2_response(const uint8_t frame[17])
{
    return (frame != NULL) && (frame[0] == 0x3fu) && ((frame[16] & 1u) != 0u) &&
           ((frame[16] >> 1u) == pf_sd_crc7(frame + 1u, 15u));
}

bool pf_sd_native_status_is_transfer_ready(uint32_t status)
{
    return (status & PF_SD_NATIVE_R1_ERROR_MASK) == 0u &&
           (status & PF_SD_NATIVE_R1_READY_FOR_DATA) != 0u &&
           (status & PF_SD_NATIVE_R1_STATE_MASK) == PF_SD_NATIVE_R1_STATE_TRAN;
}

/** @brief Advance four bit-sliced lane CRCs by one wire nibble. */
static uint64_t crc16_4bit_nibble(uint64_t reg, uint8_t nibble)
{
    uint64_t feedback = ((reg >> 60u) ^ nibble) & 0x0fu;

    return (reg << 4u) ^ (feedback << 48u) ^ (feedback << 20u) ^ feedback;
}

/*
 * The CRC step is linear, so four data bytes are folded at once: with x = reg ^ (word << 32),
 * reg' = (x << 32) ^ slice[3][x >> 56] ^ slice[2][x >> 48] ^ slice[1][x >> 40] ^ slice[0][x >> 32]
 * (byte indices 8 bits wide). slice[k][v] is v shifted through four byte steps. The 8 KiB RAM
 * tables are built on first use. This replaces a per-byte loop that was measured to be a
 * significant CPU cost, not proven to be the sole limit on 25 MHz transfers.
 *
 * Each table is kept as separate high and low uint32_t halves so every lookup is two plain
 * word loads and no 64-bit shifts are needed on the 32-bit Cortex-M cores.
 *
 * load_be32() returns the four wire-order bytes as a big-endian word on supported hosts.
 * ponytail: lazy initialization assumes serialized storage-worker calls, as before;
 * concurrent callers would need explicit initialization before starting the workers.
 * Tables are capped at 8 KiB (6 KiB more than the byte table): slicing-by-8 doubled
 * RAM without reducing compiled instructions per byte on Cortex-M33. Revisit only
 * with measured benefit and a board RAM budget, not by increasing slices blindly.
 */
static uint32_t crc16_4bit_slice_hi[4][256];
static uint32_t crc16_4bit_slice_lo[4][256];
static bool crc16_4bit_table_ready;

/** @brief Initialize the shared four-lane CRC lookup tables before first use. */
static void crc16_4bit_table_init(void)
{
    for (uint32_t slice = 0u; slice < 4u; slice++) {
        for (uint32_t index = 0u; index < 256u; index++) {
            uint64_t reg = (uint64_t)index << (32u + 8u * slice);

            for (uint32_t step = 0u; step < 8u; step++) {
                reg = crc16_4bit_nibble(reg, 0u);
            }
            crc16_4bit_slice_hi[slice][index] = (uint32_t)(reg >> 32u);
            crc16_4bit_slice_lo[slice][index] = (uint32_t)reg;
        }
    }
    crc16_4bit_table_ready = true;
}

/** @brief Fold four wire-order bytes, first byte in the most significant position. */
static inline uint64_t crc16_4bit_word(uint64_t reg, uint32_t word)
{
    uint32_t x = (uint32_t)(reg >> 32u) ^ word;
    uint32_t b3 = x >> 24u;
    uint32_t b2 = (x >> 16u) & 0xffu;
    uint32_t b1 = (x >> 8u) & 0xffu;
    uint32_t b0 = x & 0xffu;
    uint32_t hi = (uint32_t)reg ^ crc16_4bit_slice_hi[3][b3] ^ crc16_4bit_slice_hi[2][b2] ^
                  crc16_4bit_slice_hi[1][b1] ^ crc16_4bit_slice_hi[0][b0];
    uint32_t lo = crc16_4bit_slice_lo[3][b3] ^ crc16_4bit_slice_lo[2][b2] ^
                  crc16_4bit_slice_lo[1][b1] ^ crc16_4bit_slice_lo[0][b0];

    return ((uint64_t)hi << 32u) | lo;
}

static inline uint32_t load_be32(const uint8_t *data)
{
    uint32_t word;

    memcpy(&word, data, sizeof(word));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return word;
#else
    return __builtin_bswap32(word);
#endif
}

#if PF_SD_NATIVE_FUSED_COPY
static inline void store_be32(uint8_t *data, uint32_t word)
{
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
    word = __builtin_bswap32(word);
#endif
    memcpy(data, &word, sizeof(word));
}
#endif

static uint64_t crc16_4bit_block(const uint8_t block[512])
{
    uint64_t reg = 0u;

    if (!crc16_4bit_table_ready) {
        crc16_4bit_table_init();
    }
    for (size_t index = 0u; index < 512u; index += 4u) {
        reg = crc16_4bit_word(reg, load_be32(block + index));
    }
    return reg;
}

void pf_sd_native_crc16_4bit(const uint8_t block[512], uint16_t crc[4])
{
    uint64_t reg;

    if ((block == NULL) || (crc == NULL)) {
        return;
    }

    reg = crc16_4bit_block(block);
    for (uint8_t lane = 0u; lane < 4u; lane++) {
        crc[lane] = 0u;
        for (uint8_t bit = 0u; bit < 16u; bit++) {
            crc[lane] |= (uint16_t)(((reg >> (4u * bit + lane)) & 1u) << bit);
        }
    }
}

bool pf_sd_native_encode_data_block_sized(const uint8_t *block, size_t block_size,
                                         uint8_t image[PF_SD_NATIVE_IMAGE_SIZE])
{
    uint64_t reg = 0u;

    if ((block == NULL) || (image == NULL) ||
        (block_size == 0u) || (block_size > 512u) || ((block_size & 3u) != 0u)) {
        return false;
    }
    if (!crc16_4bit_table_ready) {
        crc16_4bit_table_init();
    }
    for (size_t index = 0u; index < block_size; index += 4u) {
        uint32_t word = load_be32(block + index);
        reg = crc16_4bit_word(reg, word);
    #if PF_SD_NATIVE_FUSED_COPY
        store_be32(image + index, word);
    #endif
    }
    #if !PF_SD_NATIVE_FUSED_COPY
    memcpy(image, block, block_size);
    #endif
    for (size_t index = 0u; index < 8u; index++) {
        uint8_t crc_byte = (uint8_t)(reg >> (56u - 8u * index));
        image[block_size + index] = crc_byte;
    }
    return true;
}

bool pf_sd_native_encode_data_block(const uint8_t block[512],
                                   uint8_t image[PF_SD_NATIVE_IMAGE_SIZE])
{
    return pf_sd_native_encode_data_block_sized(block, 512u, image);
}

/** @brief Decode or discard a borrowed wire image, verifying every lane CRC. */
static bool decode_data_block(const uint8_t *image, uint8_t *block, size_t block_size)
{
    uint64_t reg = 0u;
    uint64_t received = 0u;

    if ((image == NULL) ||
        (block_size == 0u) || (block_size > 512u) || ((block_size & 3u) != 0u)) {
        return false;
    }
    if (!crc16_4bit_table_ready) {
        crc16_4bit_table_init();
    }
    for (size_t index = 0u; index < block_size; index += 4u) {
        uint32_t word = load_be32(image + index);
        reg = crc16_4bit_word(reg, word);
#if PF_SD_NATIVE_FUSED_COPY
        if (block != NULL) {
            store_be32(block + index, word);
        }
#endif
    }
#if !PF_SD_NATIVE_FUSED_COPY
    if (block != NULL) {
        memcpy(block, image, block_size);
    }
#endif
    for (size_t index = 0u; index < 8u; index++) {
        received = (received << 8u) | image[block_size + index];
    }
    return received == reg;
}

bool pf_sd_native_decode_data_block_sized(const uint8_t *image, uint8_t *block,
                                         size_t block_size)
{
    return block != NULL && decode_data_block(image, block, block_size);
}

bool pf_sd_native_verify_data_block(const uint8_t image[PF_SD_NATIVE_IMAGE_SIZE])
{
    return decode_data_block(image, NULL, 512u);
}

bool pf_sd_native_decode_data_block(const uint8_t image[PF_SD_NATIVE_IMAGE_SIZE],
                                   uint8_t block[512])
{
    return pf_sd_native_decode_data_block_sized(image, block, 512u);
}

bool pf_sd_native_switch_supports_high_speed(
    const uint8_t status[PF_SD_NATIVE_SWITCH_STATUS_SIZE])
{
    return (status != NULL) && ((status[13] & 0x02u) != 0u);
}

bool pf_sd_native_switch_selected_high_speed(
    const uint8_t status[PF_SD_NATIVE_SWITCH_STATUS_SIZE])
{
    return (status != NULL) && ((status[16] & 0x0fu) == 1u);
}

bool pf_sd_native_switch_high_speed_busy(
    const uint8_t status[PF_SD_NATIVE_SWITCH_STATUS_SIZE])
{
    return status != NULL && status[17] >= 1u && (status[29] & 0x02u) != 0u;
}