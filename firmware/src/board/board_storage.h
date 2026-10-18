#ifndef PF_BOARD_STORAGE_H
#define PF_BOARD_STORAGE_H

#include "drivers/storage/bus.h"

/** @brief Prepare the selected serial-storage profile and its board-specific idle pins.
 * Returns a non-owning handle to a static, uninitialized SPI/QSPI context.
 * The caller initializes the bus through its device adapter and serializes all access.
 * Repeated preparation requires bus deinitialization and pin release first.
 * @param bus Receives the board-owned bus handle on success.
 * @return Physical result; preparation never sends device commands or acquires bus hardware.
 */
pf_storage_bus_result_t pf_board_storage_prepare(pf_storage_bus_t *bus);

/** @brief Release board-specific idle pins after the caller deinitializes the bus.
 * Does not deinitialize transport hardware, synchronize media, or send device commands.
 * Leaves pins unchanged if the physical context still owns live hardware resources.
 */
void pf_board_storage_release_pins(void);

/** @brief Read the configured physical serial clock, or zero when not initialized. */
uint32_t pf_board_storage_clock_hz(void);

#endif
