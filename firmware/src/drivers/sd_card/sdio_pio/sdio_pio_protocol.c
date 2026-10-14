#include "sdio_pio_protocol.h"
#include "sd_card_protocol.h"

#include <string.h>

uint8_t pf_sdio_pio_crc7(const uint8_t *data, size_t size)
{
    return pf_sd_card_crc7(data, size);
}

bool pf_sdio_pio_build_command(uint8_t command, uint32_t argument, uint8_t frame[6])
{
    return pf_sd_card_build_command(command, argument, frame);
}

bool pf_sdio_pio_parse_response(uint8_t command, const uint8_t frame[6],
                                   uint32_t *argument, bool check_crc)
{
    if ((frame == NULL) || (argument == NULL) ||
        ((frame[0] & 0xc0u) != 0u) || ((frame[0] & 0x3fu) != command) ||
        ((frame[5] & 1u) == 0u)) {
        return false;
    }
    if (check_crc && ((frame[5] >> 1u) != pf_sdio_pio_crc7(frame, 5u))) {
        return false;
    }

    *argument = ((uint32_t)frame[1] << 24u) |
                ((uint32_t)frame[2] << 16u) |
                ((uint32_t)frame[3] << 8u) |
                (uint32_t)frame[4];
    return true;
}

bool pf_sdio_pio_response_words_to_bytes(const uint32_t *words, uint16_t response_bits,
                                            uint8_t *response, size_t response_capacity)
{
    size_t response_size;

    if ((response_bits > 136u) || ((response_bits % 8u) != 0u)) {
        return false;
    }
    response_size = response_bits / 8u;
    if (response_size > response_capacity) {
        return false;
    }
    if (response_size == 0u) {
        return true;
    }
    if ((words == NULL) || (response == NULL)) {
        return false;
    }

    for (size_t byte_index = 0u; byte_index < response_size; byte_index++) {
        size_t word_index = byte_index / 4u;
        size_t word_byte_index = byte_index % 4u;
        size_t word_byte_count = response_size - word_index * 4u;
        if (word_byte_count > 4u) {
            word_byte_count = 4u;
        }
        size_t shift = (word_byte_count - 1u - word_byte_index) * 8u;
        response[byte_index] = (uint8_t)(words[word_index] >> shift);
    }
    return true;
}

bool pf_sdio_pio_parse_r3_response(const uint8_t frame[6], uint32_t *ocr)
{
    if ((frame == NULL) || (ocr == NULL) ||
        ((frame[0] & 0xc0u) != 0u) || ((frame[5] & 1u) == 0u)) {
        return false;
    }

    *ocr = ((uint32_t)frame[1] << 24u) |
           ((uint32_t)frame[2] << 16u) |
           ((uint32_t)frame[3] << 8u) |
           (uint32_t)frame[4];
    return true;
}

bool pf_sdio_pio_write_response_accepted(uint32_t response_word)
{
    return (response_word & 0x0fu) == 0x05u;
}

bool pf_sdio_pio_cmd8_supports_v2(uint32_t response_argument, bool *supports_v2)
{
    return pf_sd_card_cmd8_supports_v2(response_argument, supports_v2);
}

bool pf_sdio_pio_validate_r2_response(const uint8_t frame[17])
{
    return (frame != NULL) && ((frame[16] & 1u) != 0u) &&
           ((frame[16] >> 1u) == pf_sdio_pio_crc7(frame + 1u, 15u));
}

bool pf_sdio_pio_csd_sector_count(const uint8_t csd[16], uint32_t *sector_count)
{
    return pf_sd_card_csd_sector_count(csd, sector_count);
}

/*
 * The four lane CRC16s are held bit-sliced: nibble p of the register is CRC bit p
 * of lanes 0..3, so one shift/XOR advances all lanes by one data nibble.
 */
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

/* @p word holds four wire-order bytes, first byte in the most significant position. */
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

static uint8_t map_byte(const uint8_t nibble_map[16], uint8_t value)
{
    return (uint8_t)((nibble_map[value >> 4u] << 4u) | nibble_map[value & 0x0fu]);
}

void pf_sdio_pio_crc16_4bit(const uint8_t block[512], uint16_t crc[4])
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

bool pf_sdio_pio_lane_map_init(pf_sdio_pio_lane_map_t *map, const uint32_t data_gpio[4])
{
    uint32_t gpio_base = UINT32_MAX;
    uint8_t lane_at_gpio_bit[4] = { 0u, 0u, 0u, 0u };
    bool lane_assigned[4] = { false, false, false, false };

    if ((map == NULL) || (data_gpio == NULL)) {
        return false;
    }

    for (uint8_t lane = 0u; lane < 4u; lane++) {
        if (data_gpio[lane] < gpio_base) {
            gpio_base = data_gpio[lane];
        }
    }
    for (uint8_t lane = 0u; lane < 4u; lane++) {
        uint32_t gpio_bit = data_gpio[lane] - gpio_base;
        if ((gpio_bit >= 4u) || lane_assigned[gpio_bit]) {
            return false;
        }
        lane_at_gpio_bit[gpio_bit] = lane;
        lane_assigned[gpio_bit] = true;
    }

    for (uint8_t nibble = 0u; nibble < 16u; nibble++) {
        uint8_t to_gpio = 0u;
        uint8_t from_gpio = 0u;

        for (uint8_t gpio_bit = 0u; gpio_bit < 4u; gpio_bit++) {
            uint8_t lane = lane_at_gpio_bit[gpio_bit];
            to_gpio |= (uint8_t)(((nibble >> lane) & 1u) << gpio_bit);
            from_gpio |= (uint8_t)(((nibble >> gpio_bit) & 1u) << lane);
        }
        map->to_gpio[nibble] = to_gpio;
        map->from_gpio[nibble] = from_gpio;
    }
    for (uint32_t value = 0u; value < 256u; value++) {
        map->to_gpio_byte[value] = map_byte(map->to_gpio, (uint8_t)value);
        map->from_gpio_byte[value] = map_byte(map->from_gpio, (uint8_t)value);
    }
    return true;
}

bool pf_sdio_pio_encode_data_block_sized(const pf_sdio_pio_lane_map_t *map,
                                         const uint8_t *block, size_t block_size,
                                         uint8_t image[PF_SDIO_PIO_IMAGE_SIZE])
{
    uint64_t reg = 0u;

    if ((map == NULL) || (block == NULL) || (image == NULL) ||
        (block_size == 0u) || (block_size > 512u) || ((block_size & 3u) != 0u)) {
        return false;
    }
    if (!crc16_4bit_table_ready) {
        crc16_4bit_table_init();
    }

    for (size_t index = 0u; index < block_size; index += 4u) {
        reg = crc16_4bit_word(reg, load_be32(block + index));
        image[index] = map->to_gpio_byte[block[index]];
        image[index + 1u] = map->to_gpio_byte[block[index + 1u]];
        image[index + 2u] = map->to_gpio_byte[block[index + 2u]];
        image[index + 3u] = map->to_gpio_byte[block[index + 3u]];
    }
    for (size_t index = 0u; index < 8u; index++) {
        image[block_size + index] = map_byte(map->to_gpio,
                                             (uint8_t)(reg >> (56u - 8u * index)));
    }
    return true;
}

bool pf_sdio_pio_encode_data_block(const pf_sdio_pio_lane_map_t *map, const uint8_t block[512],
                                   uint8_t image[PF_SDIO_PIO_IMAGE_SIZE])
{
    return pf_sdio_pio_encode_data_block_sized(map, block, 512u, image);
}

bool pf_sdio_pio_decode_data_block_sized(const pf_sdio_pio_lane_map_t *map,
                                         const uint8_t *image, uint8_t *block,
                                         size_t block_size)
{
    uint64_t reg = 0u;
    uint64_t received = 0u;

    if ((map == NULL) || (image == NULL) || (block == NULL) ||
        (block_size == 0u) || (block_size > 512u) || ((block_size & 3u) != 0u)) {
        return false;
    }
    if (!crc16_4bit_table_ready) {
        crc16_4bit_table_init();
    }

    /*
     * The CRC update treats all four bit positions of a nibble identically, so it can
     * run on wire-order bytes and be compared with the wire-order CRC bytes; only the
     * 512 data bytes need a lane remap.
     */
    for (size_t index = 0u; index < block_size; index += 4u) {
        reg = crc16_4bit_word(reg, load_be32(image + index));
        block[index] = map->from_gpio_byte[image[index]];
        block[index + 1u] = map->from_gpio_byte[image[index + 1u]];
        block[index + 2u] = map->from_gpio_byte[image[index + 2u]];
        block[index + 3u] = map->from_gpio_byte[image[index + 3u]];
    }
    for (size_t index = 0u; index < 8u; index++) {
        received = (received << 8u) | image[block_size + index];
    }
    return received == reg;
}

bool pf_sdio_pio_decode_data_block(const pf_sdio_pio_lane_map_t *map,
                                   const uint8_t image[PF_SDIO_PIO_IMAGE_SIZE], uint8_t block[512])
{
    return pf_sdio_pio_decode_data_block_sized(map, image, block, 512u);
}

bool pf_sdio_pio_switch_supports_high_speed(
    const uint8_t status[PF_SDIO_PIO_SWITCH_STATUS_SIZE])
{
    return (status != NULL) && ((status[13] & 0x02u) != 0u);
}

bool pf_sdio_pio_switch_selected_high_speed(
    const uint8_t status[PF_SDIO_PIO_SWITCH_STATUS_SIZE])
{
    return (status != NULL) && ((status[16] & 0x0fu) == 1u);
}
