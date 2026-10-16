#ifndef PF_NOR_FLASH_H
#define PF_NOR_FLASH_H

#include "nor_flash_transport.h"

/** Raw W25Q128 device. Caller owns it and serializes all access, including transports. */
typedef struct {
    pf_nor_transport_t transport;
    uint32_t capacity_bytes;
    uint32_t erase_size;
    uint32_t page_size;
    bool initialized;
    bool quad_enabled;
} pf_nor_flash_t;

/** Identify EF 40 18 (W25Q128), wait for readiness; never erase or format.
 * Quad configuration requires a W25Q128JV-compatible 35h/50h/31h command set.
 */
pf_nor_result_t pf_nor_flash_init(pf_nor_flash_t *flash, const pf_nor_transport_t *transport);
/** Read bytes with 03h or 6Bh; quad reads require explicit quad enable first. */
pf_nor_result_t pf_nor_flash_read(pf_nor_flash_t *flash, uint32_t address,
                                 void *buffer, size_t length);
/** Program bytes, splitting at 256-byte boundaries. Caller must erase first.
 * Verify each programmed chunk. No automatic erase or 0-to-1 repair is performed.
 * Failure may leave a partial write.
 */
pf_nor_result_t pf_nor_flash_program(pf_nor_flash_t *flash, uint32_t address,
                                    const void *buffer, size_t length);
/** Erase and verify one aligned 4096-byte sector; destructive and synchronous. */
pf_nor_result_t pf_nor_flash_erase(pf_nor_flash_t *flash, uint32_t address);
/** Wait for busy to clear; raw operations do not provide filesystem atomicity. */
pf_nor_result_t pf_nor_flash_sync(pf_nor_flash_t *flash);
/** Set QE using volatile status-register write, preserving all other SR2 bits. */
pf_nor_result_t pf_nor_flash_enable_quad(pf_nor_flash_t *flash);

#endif
