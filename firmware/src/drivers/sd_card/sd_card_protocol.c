#include "sd_card_protocol.h"
#include "sd_card_block.h"

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

static void (*idle_hook)(void);

void pf_sd_card_set_idle_hook(void (*hook)(void))
{
    idle_hook = hook;
}

void pf_sd_card_idle(void)
{
    if (idle_hook != NULL) {
        idle_hook();
    }
}

bool pf_sd_card_block_argument(uint32_t block, bool block_addressed, uint32_t *argument)
{
    if ((argument == NULL) ||
        (!block_addressed && (block > UINT32_MAX / PF_SD_CARD_BLOCK_SIZE))) {
        return false;
    }
    *argument = block_addressed ? block : block * PF_SD_CARD_BLOCK_SIZE;
    return true;
}

pf_sd_card_result_t pf_sd_card_validate_transfer(uint32_t first_block, const void *buffer,
                                                uint32_t block_count, uint32_t capacity)
{
    if (block_count == 0u) {
        return PF_SD_CARD_OK;
    }
    if ((buffer == NULL) || (first_block >= capacity) ||
        (block_count > capacity - first_block)) {
        return PF_SD_CARD_INVALID_ARGUMENT;
    }
    return PF_SD_CARD_OK;
}

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

bool pf_sd_card_csd_transfer_hz(const uint8_t csd[16], uint32_t *transfer_hz)
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
    *transfer_hz = ((uint32_t)multiplier_tenths[multiplier] * unit_hz[unit]) / 10u;
    return *transfer_hz != 0u;
}

bool pf_sd_card_scr_supports_four_bit(const uint8_t scr[8])
{
    return (scr != NULL) && ((scr[1] & 0x04u) != 0u);
}

uint16_t pf_sd_card_crc16(const uint8_t *data, size_t size)
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