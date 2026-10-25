#ifndef PF_DRIVERS_BOOT_CONFIG_H
#define PF_DRIVERS_BOOT_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Size in bytes of the serialized boot-config record. */
#define PF_BOOT_CONFIG_RECORD_SIZE 16u
/** Record format version; a different value is treated as unknown and yields defaults. */
#define PF_BOOT_CONFIG_FORMAT_VERSION 1u

/** Boot-applied configuration. Zero in every field selects the build/SDK default. */
typedef struct {
    uint16_t system_clock_mhz; /**< 0 (keep the build clock), 150, 200 or 250; applied at boot. */
    uint8_t sdio_bus_mhz;      /**< 0/25 (25 MHz Default Speed) or 50 (allow High-Speed negotiation); SDIO_PIO only. */
    uint8_t spi_bus_mhz;       /**< 0 (default), 25 or 50 MHz ceiling; used only by SPI images. */
} pf_boot_config_t;

/** Fill @p config with the complete default configuration. */
void pf_boot_config_defaults(pf_boot_config_t *config);

/** Return true when every field holds a supported value. */
bool pf_boot_config_valid(const pf_boot_config_t *config);

/** IEEE 802.3 CRC-32 (reflected, init and final XOR 0xFFFFFFFF). */
uint32_t pf_boot_config_crc32(const uint8_t *data, size_t size);

/** Serialize valid configuration into @p record; returns false (record untouched) if invalid. */
bool pf_boot_config_encode(const pf_boot_config_t *config, uint8_t record[PF_BOOT_CONFIG_RECORD_SIZE]);

/**
 * Parse a stored record. Any missing, erased, malformed, unknown-version, out-of-range or CRC-invalid
 * input returns false and leaves @p config holding the complete defaults; fields are never partially applied.
 */
bool pf_boot_config_decode(const uint8_t *record, size_t size, pf_boot_config_t *config);

/**
 * @brief Read the saved configuration before the scheduler starts.
 * @param config Always filled; complete defaults unless a valid record was loaded.
 * @return true if a valid record was loaded.
 */
bool pf_boot_config_load_early(pf_boot_config_t *config);

/**
 * @brief Read the saved configuration from the designated USB settings-owner task.
 * @param config Always filled; complete defaults unless a valid record was loaded.
 * @return true if a valid record was loaded.
 */
bool pf_boot_config_load(pf_boot_config_t *config);

/**
 * @brief Validate and persist boot-applied configuration.
 * @param config Configuration to persist.
 * @return true only when the record was read back successfully.
 */
bool pf_boot_config_save(const pf_boot_config_t *config);

#endif