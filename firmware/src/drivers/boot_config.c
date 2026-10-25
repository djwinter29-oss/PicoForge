/**
 * @file boot_config.c
 * @brief Boot configuration defaults, validation, serialization and persistence facade.
 *
 * Little-endian record: magic u32 'PFCF', version u16, size u16, system_clock_mhz u16, sdio_bus_mhz u8,
 * spi_bus_mhz u8, crc32 u32 over the preceding 12 bytes.
 */
#include "drivers/boot_config.h"

#include "platform/boot_config_store.h"

#define MAGIC 0x46434650u /* "PFCF" in little-endian byte order */
#define CRC_OFFSET (PF_BOOT_CONFIG_RECORD_SIZE - 4u)

static uint32_t get_le(const uint8_t *p, unsigned bytes)
{
    uint32_t value = 0u;
    while (bytes-- > 0u) {
        value = (value << 8) | p[bytes];
    }
    return value;
}

static void put_le(uint8_t *p, unsigned bytes, uint32_t value)
{
    for (unsigned i = 0u; i < bytes; ++i) {
        p[i] = (uint8_t)(value >> (8u * i));
    }
}

void pf_boot_config_defaults(pf_boot_config_t *settings)
{
    *settings = (pf_boot_config_t){0};
}

bool pf_boot_config_valid(const pf_boot_config_t *settings)
{
    return settings != NULL &&
           (settings->system_clock_mhz == 0u || settings->system_clock_mhz == 150u ||
            settings->system_clock_mhz == 200u || settings->system_clock_mhz == 250u) &&
           (settings->sdio_bus_mhz == 0u || settings->sdio_bus_mhz == 25u || settings->sdio_bus_mhz == 50u) &&
           (settings->spi_bus_mhz == 0u || settings->spi_bus_mhz == 25u || settings->spi_bus_mhz == 50u);
}

uint32_t pf_boot_config_crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xffffffffu;
    while (size-- > 0u) {
        crc ^= *data++;
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

bool pf_boot_config_encode(const pf_boot_config_t *settings, uint8_t record[PF_BOOT_CONFIG_RECORD_SIZE])
{
    if (!pf_boot_config_valid(settings) || record == NULL) {
        return false;
    }
    put_le(record, 4u, MAGIC);
    put_le(record + 4, 2u, PF_BOOT_CONFIG_FORMAT_VERSION);
    put_le(record + 6, 2u, PF_BOOT_CONFIG_RECORD_SIZE);
    put_le(record + 8, 2u, settings->system_clock_mhz);
    record[10] = settings->sdio_bus_mhz;
    record[11] = settings->spi_bus_mhz;
    put_le(record + CRC_OFFSET, 4u, pf_boot_config_crc32(record, CRC_OFFSET));
    return true;
}

bool pf_boot_config_decode(const uint8_t *record, size_t size, pf_boot_config_t *settings)
{
    pf_boot_config_t parsed;
    if (settings == NULL) {
        return false;
    }
    pf_boot_config_defaults(settings);
    if (record == NULL || size < PF_BOOT_CONFIG_RECORD_SIZE || get_le(record, 4u) != MAGIC ||
        get_le(record + 4, 2u) != PF_BOOT_CONFIG_FORMAT_VERSION || get_le(record + 6, 2u) != PF_BOOT_CONFIG_RECORD_SIZE ||
        get_le(record + CRC_OFFSET, 4u) != pf_boot_config_crc32(record, CRC_OFFSET)) {
        return false;
    }
    parsed = (pf_boot_config_t){
        .system_clock_mhz = (uint16_t)get_le(record + 8, 2u),
        .sdio_bus_mhz = record[10],
        .spi_bus_mhz = record[11],
    };
    if (!pf_boot_config_valid(&parsed)) {
        return false;
    }
    *settings = parsed;
    return true;
}

bool pf_boot_config_load_early(pf_boot_config_t *config)
{
    uint8_t record[PF_BOOT_CONFIG_RECORD_SIZE];
    if (config == NULL) {
        return false;
    }
    pf_boot_config_defaults(config);
    if (!pf_platform_boot_config_read_early(record, sizeof(record))) {
        return false;
    }
    return pf_boot_config_decode(record, sizeof(record), config);
}

bool pf_boot_config_load(pf_boot_config_t *config)
{
    uint8_t record[PF_BOOT_CONFIG_RECORD_SIZE];
    if (config == NULL) {
        return false;
    }
    pf_boot_config_defaults(config);
    if (!pf_platform_boot_config_read(record, sizeof(record))) {
        return false;
    }
    return pf_boot_config_decode(record, sizeof(record), config);
}

bool pf_boot_config_save(const pf_boot_config_t *config)
{
    uint8_t record[PF_BOOT_CONFIG_RECORD_SIZE];
    return pf_boot_config_encode(config, record) &&
           pf_platform_boot_config_write(record, sizeof(record));
}