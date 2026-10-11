#include "sd_card_protocol.h"
#include "sd_card_block.h"

uint8_t pf_sd_card_crc7(const uint8_t *data, size_t size)
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

bool pf_sd_card_build_command(uint8_t command, uint32_t argument, uint8_t frame[6])
{
    if ((command > 63u) || (frame == NULL)) {
        return false;
    }
    frame[0] = (uint8_t)(0x40u | command);
    frame[1] = (uint8_t)(argument >> 24u);
    frame[2] = (uint8_t)(argument >> 16u);
    frame[3] = (uint8_t)(argument >> 8u);
    frame[4] = (uint8_t)argument;
    frame[5] = (uint8_t)((pf_sd_card_crc7(frame, 5u) << 1u) | 1u);
    return true;
}

bool pf_sd_card_cmd8_supports_v2(uint32_t response_argument, bool *supports_v2)
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

bool pf_sd_card_csd_sector_count(const uint8_t csd[16], uint32_t *sector_count)
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
                  (1u << read_bl_len) / PF_SD_CARD_BLOCK_SIZE;
    } else {
        return false;
    }

    if ((sectors == 0u) || (sectors > UINT32_MAX)) {
        return false;
    }
    *sector_count = (uint32_t)sectors;
    return true;
}

uint16_t pf_sd_card_crc16(const uint8_t *data, size_t size)
{
    uint16_t crc = 0u;

    if (data == NULL) {
        return 0u;
    }
    for (size_t byte_index = 0u; byte_index < size; byte_index++) {
        crc ^= (uint16_t)data[byte_index] << 8u;
        for (uint8_t bit_index = 0u; bit_index < 8u; bit_index++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1u) ^ 0x1021u)
                                  : (uint16_t)(crc << 1u);
        }
    }
    return crc;
}