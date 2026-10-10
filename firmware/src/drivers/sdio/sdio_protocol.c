#include "sdio_protocol.h"

uint8_t picoforge_sdio_crc7(const uint8_t *data, size_t size)
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

bool picoforge_sdio_build_command(uint8_t command, uint32_t argument, uint8_t frame[6])
{
    if ((command > 63u) || (frame == NULL)) {
        return false;
    }

    frame[0] = (uint8_t)(0x40u | command);
    frame[1] = (uint8_t)(argument >> 24u);
    frame[2] = (uint8_t)(argument >> 16u);
    frame[3] = (uint8_t)(argument >> 8u);
    frame[4] = (uint8_t)argument;
    frame[5] = (uint8_t)((picoforge_sdio_crc7(frame, 5u) << 1u) | 1u);
    return true;
}

bool picoforge_sdio_parse_response(uint8_t command, const uint8_t frame[6],
                                   uint32_t *argument, bool check_crc)
{
    if ((frame == NULL) || (argument == NULL) ||
        ((frame[0] & 0xc0u) != 0u) || ((frame[0] & 0x3fu) != command) ||
        ((frame[5] & 1u) == 0u)) {
        return false;
    }
    if (check_crc && ((frame[5] >> 1u) != picoforge_sdio_crc7(frame, 5u))) {
        return false;
    }

    *argument = ((uint32_t)frame[1] << 24u) |
                ((uint32_t)frame[2] << 16u) |
                ((uint32_t)frame[3] << 8u) |
                (uint32_t)frame[4];
    return true;
}

bool picoforge_sdio_validate_r2_response(const uint8_t frame[17])
{
    return (frame != NULL) && ((frame[16] & 1u) != 0u) &&
           ((frame[16] >> 1u) == picoforge_sdio_crc7(frame, 16u));
}

bool picoforge_sdio_csd_sector_count(const uint8_t csd[16], uint32_t *sector_count)
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

void picoforge_sdio_crc16_4bit(const uint8_t block[512], uint16_t crc[4])
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

void picoforge_sdio_pack_data_block(const uint8_t block[512],
                                    uint32_t words[PICOFORGE_SDIO_TRANSFER_WORDS])
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
    picoforge_sdio_crc16_4bit(block, crc);
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

bool picoforge_sdio_unpack_data_block(const uint32_t words[PICOFORGE_SDIO_TRANSFER_WORDS],
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

    picoforge_sdio_crc16_4bit(block, expected_crc);
    for (uint8_t lane = 0u; lane < 4u; lane++) {
        if (received_crc[lane] != expected_crc[lane]) {
            return false;
        }
    }
    return true;
}