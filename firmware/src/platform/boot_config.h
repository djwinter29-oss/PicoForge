/**
 * @file boot_config.h
 * @brief Boot settings loaded before scheduling, then owned by the USB task.
 *
 * Both profile getters and their MHz wrappers share first-use loading: normally the start of main() loads flash
 * before scheduling; a running task can also load it. Unsafe first calls return defaults and leave loading pending.
 *
 * This API is not thread-safe. After boot, mutations and saves must be serialized in the USB task.
 * This module must be the only flash_safe_execute() caller; tasks must not be created or deleted during a save
 * or while lockout reclamation remains pending, because reclamation is detected through the task count.
 */

#ifndef PF_DRIVERS_BOOT_CONFIG_H
#define PF_DRIVERS_BOOT_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/** Wi-Fi radio master-switch bit. */
#define PF_WIRELESS_WIFI 1u
/** BLE radio master-switch bit. */
#define PF_WIRELESS_BLE 2u
/**
 * @brief Copy the saved NUL-terminated SSID (up to 32 UTF-8 bytes).
 * @param ssid Output buffer of at least 33 bytes.
 */
void pf_boot_config_wifi_ssid(char ssid[33]);
/**
 * @brief Stage an SSID in RAM; an empty string clears it. Persist with pf_boot_config_save().
 * @param ssid NUL-terminated UTF-8 string of at most 32 bytes.
 * @return false for invalid UTF-8, an overlength string, or when first-use loading is unsafe.
 */
bool pf_boot_config_set_wifi_ssid(const char *ssid);
/**
 * @brief Copy the saved private WPA2 passphrase; never expose it in a host report.
 * @param password Output buffer of at least 64 bytes.
 */
void pf_boot_config_wifi_password(char password[64]);
/**
 * @brief Stage a WPA2 passphrase in RAM; empty clears it, otherwise it must be 8-63 printable ASCII characters.
 * Persist with pf_boot_config_save().
 * @param password NUL-terminated passphrase.
 * @return false for an invalid passphrase or when first-use loading is unsafe.
 */
bool pf_boot_config_set_wifi_password(const char *password);
/**
 * @brief Copy the saved private BLE HMAC-SHA256 key; never expose it in a host report.
 * @param key Output buffer of at least 32 bytes.
 */
void pf_boot_config_ble_key(uint8_t key[32]);
/**
 * @brief Stage a BLE key in RAM; an all-zero key clears it. Persist with pf_boot_config_save().
 * @param key 32-byte key.
 * @return false when the pointer is null or first-use loading is unsafe.
 */
bool pf_boot_config_set_ble_key(const uint8_t key[32]);

/** @brief Return the PF_WIRELESS_* radios allowed to start at boot; both by default, independent of credentials. */
uint8_t pf_boot_config_radios(void);
/**
 * @brief Allow PF_WIRELESS_* radios in RAM; persist with pf_boot_config_save(), applied at the next boot.
 * @return false (configuration unchanged) for unknown bits or when first-use loading is unsafe.
 */
bool pf_boot_config_set_radios(uint8_t radios);

/** User-selected storage bus speed profile; the storage backend maps it to its own bus clock. */
typedef enum {
    PF_STORAGE_SPEED_DEFAULT = 0, /**< 25 MHz Default Speed; used when nothing is saved. */
    PF_STORAGE_SPEED_HIGH = 1, /**< 50 MHz High-Speed; SDIO falls back to 25 MHz if the card refuses. */
} pf_storage_speed_t;

/** User-selected system clock profile; flash stores its ID, while the HID protocol carries MHz. */
typedef enum {
    PF_SYSCLK_DEFAULT = 0, /**< Keep the SDK/build clock; used when nothing is saved. */
    PF_SYSCLK_150MHZ = 1, /**< 150 MHz. */
    PF_SYSCLK_200MHZ = 2, /**< 200 MHz. */
    PF_SYSCLK_250MHZ = 3, /**< 250 MHz. */
} pf_sysclk_profile_t;

/**
 * @brief Convert a system clock profile to MHz.
 * @return 0 (keep the SDK/build clock) for PF_SYSCLK_DEFAULT or an unknown profile, otherwise 150, 200 or 250.
 */
static inline uint16_t pf_sysclk_profile_mhz(pf_sysclk_profile_t profile)
{
    static const uint16_t mhz[] = {0u, 150u, 200u, 250u};
    return (unsigned)profile < sizeof(mhz) / sizeof(mhz[0]) ? mhz[profile] : 0u;
}

/**
 * @brief Convert a storage speed profile to its requested bus clock in MHz, not the achieved rate.
 * @return 50 for PF_STORAGE_SPEED_HIGH, otherwise 25 (Default Speed, also used for an unknown profile).
 */
static inline uint8_t pf_storage_speed_mhz(pf_storage_speed_t speed)
{
    return speed == PF_STORAGE_SPEED_HIGH ? 50u : 25u;
}

/**
 * @brief Return the configured system clock profile.
 */
pf_sysclk_profile_t pf_boot_config_sysclk(void);

/**
 * @brief Change the configured system clock in RAM; persist with pf_boot_config_save(), applied at the next boot.
 * @return false (configuration unchanged) for an unknown profile or when first-use loading is unsafe.
 */
bool pf_boot_config_set_sysclk(pf_sysclk_profile_t profile);

/** @brief Return the configured storage speed profile. */
pf_storage_speed_t pf_boot_config_storage_speed(void);

/** Return the configured system clock in MHz, or 0 to keep the SDK/build clock.
 * Reads the boot configuration, not the currently running hardware clock; first-use loading rules apply.
 */
static inline uint16_t pf_get_sysclk_mhz(void)
{
    return pf_sysclk_profile_mhz(pf_boot_config_sysclk());
}

/** Return the configured storage target in MHz (25 or 50), not the achieved bus clock. */
static inline uint8_t pf_get_storage_speed_mhz(void)
{
    return pf_storage_speed_mhz(pf_boot_config_storage_speed());
}

/**
 * @brief Change the configured storage speed in RAM; persist with pf_boot_config_save(), applied at the next boot.
 * @return false (configuration unchanged) for an unknown profile or when first-use loading is unsafe.
 */
bool pf_boot_config_set_storage_speed(pf_storage_speed_t speed);

/**
 * @brief Persist the configuration to flash from a task context.
 *
 * Requires the scheduler running, not an ISR; reentrant saves are rejected. Profiles, radio switches and credentials
 * are committed as one unit: an interrupted write loads either the complete new configuration or defaults.
 * @return true when unchanged or when the SDK operation succeeded and readback matched.
 * False means unconfirmed, not necessarily unchanged: erase/program can complete before an SDK exit timeout.
 * On failure, staged values are discarded so the next safe access reloads flash; callers may explicitly re-stage.
 */
bool pf_boot_config_save(void);

/**
 * @brief Drop all staged, unsaved values; the next safe access reloads the saved configuration from flash.
 *
 * Callers that stage several values call this when they do not save them, so a rejected request changes nothing.
 */
void pf_boot_config_discard(void);

#endif
