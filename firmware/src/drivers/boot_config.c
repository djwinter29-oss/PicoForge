/**
 * @file boot_config.c
 * @brief Boot configuration: one RAM copy, its v1 flash record codec, and the persistence facade.
 */
#include "drivers/boot_config.h"

#include <stddef.h>

#include "platform/boot_config_store.h"

#define MAGIC 0x46434650u /* "PFCF" in little-endian byte order */
#define FORMAT_VERSION 1u

/**
 * @brief v1 flash record, stored little-endian.
 *
 * ponytail: written and read as raw memory, which matches the flash byte order only on a little-endian CPU (RP2040,
 * RP2350 and the host tests). Every field is naturally aligned, so there is no padding; the static assert guards the
 * layout. Upgrade path for a big-endian target: serialize each field explicitly.
 */
typedef struct {
    uint32_t magic; /**< MAGIC. */
    uint16_t version; /**< FORMAT_VERSION. */
    uint16_t size; /**< sizeof(record_t). */
    uint16_t sysclk_mhz; /**< 0 (SDK/build clock), 150, 200 or 250. */
    uint8_t sdio_bus_mhz; /**< SD speed, written as 25 Default or 50 High; older firmware may leave 0 (Default). */
    uint8_t spi_bus_mhz; /**< Same value as sdio_bus_mhz; either at 50 reads as High. */
    uint32_t crc; /**< IEEE CRC-32 of all preceding bytes. */
} record_t;

_Static_assert(sizeof(record_t) == 16u && offsetof(record_t, crc) == 12u, "v1 record layout");

/** RAM copy of the configuration; all-zero is the complete default. */
typedef struct {
    pf_sysclk_profile_t sysclk; /**< System clock profile. */
    pf_storage_speed_t sd_speed; /**< Shared SD-card speed profile. */
} boot_config_t;

static boot_config_t config; /**< Configuration returned by the getters and written by save. */
static bool loaded; /**< The saved record has been read once. */

static bool sd_field_valid(uint8_t mhz)
{
    return mhz == 0u || mhz == 25u || mhz == 50u;
}

/** @brief IEEE 802.3 CRC-32 (reflected, init and final XOR 0xFFFFFFFF). */
static uint32_t crc32(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t crc = 0xffffffffu;
    while (size-- > 0u) {
        crc ^= *bytes++;
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static void encode(const boot_config_t *in, record_t *record)
{
    const uint8_t sd_mhz = pf_storage_speed_mhz(in->sd_speed);
    *record = (record_t){
        .magic = MAGIC,
        .version = FORMAT_VERSION,
        .size = sizeof(record_t),
        .sysclk_mhz = pf_sysclk_profile_mhz(in->sysclk),
        .sdio_bus_mhz = sd_mhz,
        .spi_bus_mhz = sd_mhz,
    };
    record->crc = crc32(record, offsetof(record_t, crc));
}

/** @brief Parse a record; anything malformed, unknown or out of range yields false and leaves @p out untouched. */
static bool decode(const record_t *record, boot_config_t *out)
{
    pf_sysclk_profile_t sysclk = PF_SYSCLK_DEFAULT;
    while (sysclk < PF_SYSCLK_250MHZ && pf_sysclk_profile_mhz(sysclk) != record->sysclk_mhz) {
        ++sysclk;
    }
    if (record->magic != MAGIC || record->version != FORMAT_VERSION || record->size != sizeof(record_t) ||
        record->crc != crc32(record, offsetof(record_t, crc)) || pf_sysclk_profile_mhz(sysclk) != record->sysclk_mhz ||
        !sd_field_valid(record->sdio_bus_mhz) || !sd_field_valid(record->spi_bus_mhz)) {
        return false;
    }
    *out = (boot_config_t){
        .sysclk = sysclk,
        .sd_speed = record->sdio_bus_mhz == 50u || record->spi_bus_mhz == 50u ? PF_STORAGE_SPEED_HIGH :
                                                                               PF_STORAGE_SPEED_DEFAULT,
    };
    return true;
}

/**
 * @brief Load the saved record into the RAM copy once.
 *
 * ponytail: lazy load on the first getter. The boot clock hook runs before the scheduler, so in firmware the first
 * call always takes the lock-free early read; the task-context read only serves hosts/tests that skip the hook. A
 * failed read latches defaults until reboot, which is acceptable because the record only changes through save().
 */
static void ensure_loaded(void)
{
    record_t record;
    if (loaded) {
        return;
    }
    loaded = true;
    if (pf_platform_boot_config_read_early((uint8_t *)&record, sizeof(record)) ||
        pf_platform_boot_config_read((uint8_t *)&record, sizeof(record))) {
        (void)decode(&record, &config);
    }
}

pf_sysclk_profile_t pf_boot_config_sysclk(void)
{
    ensure_loaded();
    return config.sysclk;
}

bool pf_boot_config_set_sysclk(pf_sysclk_profile_t profile)
{
    ensure_loaded();
    if ((unsigned)profile > PF_SYSCLK_250MHZ) {
        return false;
    }
    config.sysclk = profile;
    return true;
}

pf_storage_speed_t pf_boot_config_storage_speed(void)
{
    ensure_loaded();
    return config.sd_speed;
}

bool pf_boot_config_set_storage_speed(pf_storage_speed_t speed)
{
    ensure_loaded();
    if (speed != PF_STORAGE_SPEED_DEFAULT && speed != PF_STORAGE_SPEED_HIGH) {
        return false;
    }
    config.sd_speed = speed;
    return true;
}

bool pf_boot_config_save(void)
{
    record_t record;
    ensure_loaded();
    encode(&config, &record);
    return pf_platform_boot_config_write((const uint8_t *)&record, sizeof(record));
}
