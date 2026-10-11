#include "sdio_protocol.h"

uint8_t pf_sdio_crc7(const uint8_t *data, size_t size)
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

bool pf_sdio_build_command(uint8_t command, uint32_t argument, uint8_t frame[6])
{
    if ((command > 63u) || (frame == NULL)) {
        return false;
    }

    frame[0] = (uint8_t)(0x40u | command);
    frame[1] = (uint8_t)(argument >> 24u);
    frame[2] = (uint8_t)(argument >> 16u);
    frame[3] = (uint8_t)(argument >> 8u);
    frame[4] = (uint8_t)argument;
    frame[5] = (uint8_t)((pf_sdio_crc7(frame, 5u) << 1u) | 1u);
    return true;
}

bool pf_sdio_parse_response(uint8_t command, const uint8_t frame[6],
                                   uint32_t *argument, bool check_crc)
{
    if ((frame == NULL) || (argument == NULL) ||
        ((frame[0] & 0xc0u) != 0u) || ((frame[0] & 0x3fu) != command) ||
        ((frame[5] & 1u) == 0u)) {
        return false;
    }
    if (check_crc && ((frame[5] >> 1u) != pf_sdio_crc7(frame, 5u))) {
        return false;
    }

    *argument = ((uint32_t)frame[1] << 24u) |
                ((uint32_t)frame[2] << 16u) |
                ((uint32_t)frame[3] << 8u) |
                (uint32_t)frame[4];
    return true;
}

bool pf_sdio_response_words_to_bytes(const uint32_t *words, uint16_t response_bits,
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

bool pf_sdio_parse_r3_response(const uint8_t frame[6], uint32_t *ocr)
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

bool pf_sdio_write_response_accepted(uint32_t response_word)
{
    return (response_word & 0x0fu) == 0x05u;
}

bool pf_sdio_cmd8_supports_v2(uint32_t response_argument, bool *supports_v2)
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

bool pf_sdio_validate_r2_response(const uint8_t frame[17])
{
    return (frame != NULL) && ((frame[16] & 1u) != 0u) &&
           ((frame[16] >> 1u) == pf_sdio_crc7(frame, 16u));
}

bool pf_sdio_csd_sector_count(const uint8_t csd[16], uint32_t *sector_count)
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
                  (1u << read_bl_len) / 512u;
    } else {
        return false;
    }

    if ((sectors == 0u) || (sectors > UINT32_MAX)) {
        return false;
    }
    *sector_count = (uint32_t)sectors;
    return true;
}

void pf_sdio_crc16_4bit(const uint8_t block[512], uint16_t crc[4])
{
    if ((block == NULL) || (crc == NULL)) {
        return;
    }

    for (uint8_t lane = 0u; lane < 4u; lane++) {
        crc[lane] = 0u;
    }

    for (size_t byte_index = 0u; byte_index < 512u; byte_index++) {
        uint8_t nibbles[2] = {
            (uint8_t)(block[byte_index] >> 4u),
            (uint8_t)(block[byte_index] & 0x0fu),
        };

        for (size_t nibble_index = 0u; nibble_index < 2u; nibble_index++) {
            for (uint8_t lane = 0u; lane < 4u; lane++) {
                bool feedback = (((crc[lane] >> 15u) & 1u) != 0u) ^
                                (((nibbles[nibble_index] >> lane) & 1u) != 0u);
                crc[lane] <<= 1u;
                if (feedback) {
                    crc[lane] ^= 0x1021u;
                }
            }
        }
    }
}

void pf_sdio_pack_data_block(const uint8_t block[512],
                                    uint32_t words[PF_SDIO_TRANSFER_WORDS])
{
    uint16_t crc[4];

    if ((block == NULL) || (words == NULL)) {
        return;
    }

    for (size_t word_index = 0u; word_index < 128u; word_index++) {
        uint32_t word = 0u;
        for (size_t byte_index = 0u; byte_index < 4u; byte_index++) {
            uint8_t value = block[word_index * 4u + byte_index];
            word |= (uint32_t)(value >> 4u) << (28u - byte_index * 8u);
            word |= (uint32_t)(value & 0x0fu) << (24u - byte_index * 8u);
        }
        words[word_index] = word;
    }

    words[128] = 0u;
    words[129] = 0u;
    pf_sdio_crc16_4bit(block, crc);
    for (size_t cycle = 0u; cycle < 16u; cycle++) {
        uint8_t nibble = 0u;
        for (uint8_t lane = 0u; lane < 4u; lane++) {
            nibble |= (uint8_t)(((crc[lane] >> (15u - cycle)) & 1u) << lane);
        }
        size_t word_index = 128u + cycle / 8u;
        size_t shift = 28u - (cycle % 8u) * 4u;
        words[word_index] |= (uint32_t)nibble << shift;
    }
}

bool pf_sdio_unpack_data_block(const uint32_t words[PF_SDIO_TRANSFER_WORDS],
                                      uint8_t block[512])
{
    uint16_t expected_crc[4];
    uint16_t received_crc[4] = { 0u, 0u, 0u, 0u };

    if ((words == NULL) || (block == NULL)) {
        return false;
    }

    for (size_t word_index = 0u; word_index < 128u; word_index++) {
        uint32_t word = words[word_index];
        for (size_t byte_index = 0u; byte_index < 4u; byte_index++) {
            uint8_t high_nibble = (uint8_t)((word >> (28u - byte_index * 8u)) & 0x0fu);
            uint8_t low_nibble = (uint8_t)((word >> (24u - byte_index * 8u)) & 0x0fu);
            block[word_index * 4u + byte_index] = (uint8_t)((high_nibble << 4u) | low_nibble);
        }
    }

    for (size_t word_index = 0u; word_index < 2u; word_index++) {
        uint32_t word = words[128u + word_index];
        for (size_t nibble_index = 0u; nibble_index < 8u; nibble_index++) {
            uint8_t nibble = (uint8_t)((word >> (28u - nibble_index * 4u)) & 0x0fu);
            for (uint8_t lane = 0u; lane < 4u; lane++) {
                received_crc[lane] = (uint16_t)((received_crc[lane] << 1u) |
                    ((nibble >> lane) & 1u));
            }
        }
    }

    pf_sdio_crc16_4bit(block, expected_crc);
    for (uint8_t lane = 0u; lane < 4u; lane++) {
        if (received_crc[lane] != expected_crc[lane]) {
            return false;
        }
    }
    return true;
}

bool pf_sdio_remap_data_lanes(uint32_t words[PF_SDIO_TRANSFER_WORDS],
                              const uint32_t data_gpio[4], bool to_gpio_order)
{
    uint32_t gpio_base = UINT32_MAX;
    uint8_t lane_at_gpio_bit[4] = { 0u, 0u, 0u, 0u };
    bool lane_assigned[4] = { false, false, false, false };

    if ((words == NULL) || (data_gpio == NULL)) {
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

    for (size_t word_index = 0u; word_index < PF_SDIO_TRANSFER_WORDS; word_index++) {
        uint32_t remapped_word = 0u;
        for (uint8_t nibble_index = 0u; nibble_index < 8u; nibble_index++) {
            uint8_t shift = (uint8_t)(28u - nibble_index * 4u);
            uint8_t nibble = (uint8_t)((words[word_index] >> shift) & 0x0fu);
            uint8_t remapped_nibble = 0u;
            for (uint8_t gpio_bit = 0u; gpio_bit < 4u; gpio_bit++) {
                uint8_t lane = lane_at_gpio_bit[gpio_bit];
                if (to_gpio_order) {
                    remapped_nibble |= (uint8_t)(((nibble >> lane) & 1u) << gpio_bit);
                } else {
                    remapped_nibble |= (uint8_t)(((nibble >> gpio_bit) & 1u) << lane);
                }
            }
            remapped_word |= (uint32_t)remapped_nibble << shift;
        }
        words[word_index] = remapped_word;
    }

    return true;
}