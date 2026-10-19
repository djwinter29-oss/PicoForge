#ifndef PF_PERSISTENT_MEMORY_SETTINGS_H
#define PF_PERSISTENT_MEMORY_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Size in bytes of the serialized settings record. */
#define PF_SETTINGS_RECORD_SIZE 16u
/** Record format version; a different value is treated as unknown and yields defaults. */
#define PF_SETTINGS_FORMAT_VERSION 1u

/** Persistent user settings. Zero in every field selects the build/SDK default. */
typedef struct {
    uint16_t system_clock_mhz; /**< 0 (keep the build clock), 150, 200 or 250; applied at boot, see docs/settings.md. */
    uint8_t sdio_bus_mhz;      /**< 0/25 (25 MHz Default Speed) or 50 (allow High-Speed negotiation); SDIO_PIO only. */
    uint8_t spi_bus_mhz;       /**< 0 (default), 25 or 50 MHz ceiling; used only by SPI images. */
} pf_settings_t;

/** Fill @p settings with the complete default configuration. */
void pf_settings_defaults(pf_settings_t *settings);

/** Return true when every field holds a supported value. */
bool pf_settings_valid(const pf_settings_t *settings);

/** IEEE 802.3 CRC-32 (reflected, init and final XOR 0xFFFFFFFF). */
uint32_t pf_settings_crc32(const uint8_t *data, size_t size);

/** Serialize valid settings into @p record; returns false (record untouched) if invalid. */
bool pf_settings_encode(const pf_settings_t *settings, uint8_t record[PF_SETTINGS_RECORD_SIZE]);

/**
 * Parse a stored record. Any missing, erased, malformed, unknown-version, out-of-range or CRC-invalid
 * input returns false and leaves @p settings holding the complete defaults; fields are never partially applied.
 */
bool pf_settings_decode(const uint8_t *record, size_t size, pf_settings_t *settings);

#endif