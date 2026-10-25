#ifndef PF_PLATFORM_BOOT_CONFIG_STORE_H
#define PF_PLATFORM_BOOT_CONFIG_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hardware/flash.h"

/** Flash offset of the reserved boot-config sector, derived from the SDK board definition. */
#define PF_PLATFORM_BOOT_CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

/**
 * @brief Read opaque boot-configuration bytes before the scheduler starts.
 *
 * Internal persistence interface for drivers/boot_config.c; application and board code should use that facade.
 * @param data Destination buffer.
 * @param size Number of bytes to read; must fit within one flash page.
 * @return true if the request was valid and the scheduler has not started.
 */
bool pf_platform_boot_config_read_early(uint8_t *data, size_t size);

/**
 * @brief Read opaque boot-configuration bytes from a running task context.
 *
 * Runtime access is owned by the USB task through drivers/boot_config.c.
 * @param data Destination buffer.
 * @param size Number of bytes to read; must fit within one flash page.
 * @return true if the request was valid and the scheduler is running.
 */
bool pf_platform_boot_config_read(uint8_t *data, size_t size);

/**
 * @brief Erase/program the reserved configuration sector with opaque bytes via flash_safe_execute.
 *
 * Call through drivers/boot_config.c from the USB task; flash operations temporarily stall USB service.
 * @param data Bytes to store; length must fit within one flash page.
 * @param size Number of bytes to store.
 * @return true only after the bytes read back identically.
 */
bool pf_platform_boot_config_write(const uint8_t *data, size_t size);

#endif
