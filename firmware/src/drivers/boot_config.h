#ifndef PF_DRIVERS_BOOT_CONFIG_H
#define PF_DRIVERS_BOOT_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/** User-selected storage bus speed profile; the storage backend maps it to its own bus clock. */
typedef enum {
    PF_STORAGE_SPEED_DEFAULT = 0, /**< 25 MHz Default Speed; used when nothing is saved. */
    PF_STORAGE_SPEED_HIGH = 1, /**< 50 MHz High-Speed; SDIO falls back to 25 MHz if the card refuses. */
} pf_storage_speed_t;

/** User-selected system clock profile; the flash record and HID protocol carry it as MHz. */
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
 *
 * The first call loads the saved record and must run before the scheduler starts (the boot clock hook).
 */
pf_sysclk_profile_t pf_boot_config_sysclk(void);

/**
 * @brief Change the configured system clock in RAM; persist with pf_boot_config_save(), applied at the next boot.
 * @return false (configuration unchanged) for an unknown profile.
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
 * @return false (configuration unchanged) for an unknown profile.
 */
bool pf_boot_config_set_storage_speed(pf_storage_speed_t speed);

/**
 * @brief Persist the configuration to flash from a task context.
 * @return true only when the record was read back successfully.
 */
bool pf_boot_config_save(void);

#endif
