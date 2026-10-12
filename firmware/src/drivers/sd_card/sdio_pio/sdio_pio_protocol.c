#include "sdio_pio_protocol.h"
#include "sd_card_protocol.h"

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
#define CRC16_TAPS ((1ull << 48u) | (1ull << 20u) | 1ull) /* x^12, x^5, x^0 of 0x1021 */

static uint64_t crc16_4bit_nibble(uint64_t reg, uint8_t nibble)
{
    uint64_t feedback = ((reg >> 60u) ^ nibble) & 0x0fu;

    return (reg << 4u) ^ (feedback * CRC16_TAPS);
}

static uint64_t crc16_4bit_byte(uint64_t reg, uint8_t value)
{
    return crc16_4bit_nibble(crc16_4bit_nibble(reg, (uint8_t)(value >> 4u)), value & 0x0fu);
}

static uint64_t crc16_4bit_block(const uint8_t block[512])
{
    uint64_t reg = 0u;

    for (size_t index = 0u; index < 512u; index++) {
        reg = crc16_4bit_byte(reg, block[index]);
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
    return true;
}

bool pf_sdio_pio_encode_data_block(const pf_sdio_pio_lane_map_t *map, const uint8_t block[512],
                                   uint8_t image[PF_SDIO_PIO_IMAGE_SIZE])
{
    uint64_t reg = 0u;

    if ((map == NULL) || (block == NULL) || (image == NULL)) {
        return false;
    }

    for (size_t index = 0u; index < 512u; index++) {
        reg = crc16_4bit_byte(reg, block[index]);
        image[index] = map_byte(map->to_gpio, block[index]);
    }
    for (size_t index = 0u; index < 8u; index++) {
        image[512u + index] = map_byte(map->to_gpio, (uint8_t)(reg >> (56u - 8u * index)));
    }
    return true;
}

bool pf_sdio_pio_decode_data_block(const pf_sdio_pio_lane_map_t *map,
                                   const uint8_t image[PF_SDIO_PIO_IMAGE_SIZE], uint8_t block[512])
{
    uint64_t reg = 0u;
    uint8_t mismatch = 0u;

    if ((map == NULL) || (image == NULL) || (block == NULL)) {
        return false;
    }

    for (size_t index = 0u; index < 512u; index++) {
        block[index] = map_byte(map->from_gpio, image[index]);
        reg = crc16_4bit_byte(reg, block[index]);
    }
    for (size_t index = 0u; index < 8u; index++) {
        mismatch |= (uint8_t)(map_byte(map->from_gpio, image[512u + index]) ^
                              (uint8_t)(reg >> (56u - 8u * index)));
    }
    return mismatch == 0u;
}
