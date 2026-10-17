#ifndef PF_STORAGE_SERIAL_H
#define PF_STORAGE_SERIAL_H

#include "bus.h"

/** One raw serial phase; buffers remain borrowed until transfer returns.
 * Buffered phases contain whole bytes, MSB first. With neither buffer, drive ones for bit_count bits.
 * Lane/direction changes do not release CS. Four-lane phases are half duplex.
 */
typedef struct {
    const uint8_t *tx; /**< Optional output; NULL drives ones. */
    uint8_t *rx; /**< Optional input; NULL discards. */
    size_t bit_count; /**< Bits across all lanes; whole lane symbols, nonzero. */
    uint8_t lanes; /**< One or four data lanes. */
} pf_storage_serial_phase_t;

/** Raw serial operations; no opcodes, addresses, device status, or retries. */
struct pf_storage_serial_ops {
    pf_storage_bus_result_t (*transfer)(void *context, const pf_storage_serial_phase_t *phases,
                                             size_t count); /**< One CS boundary, cleanup on failure. */
    pf_storage_bus_result_t (*select)(void *context, bool selected); /**< Optional manual CS control. */
    pf_storage_bus_result_t (*exchange)(void *context, const uint8_t *tx,
                                             uint8_t *rx, size_t length); /**< Optional raw single-lane bytes. */
    uint8_t lanes; /**< Maximum supported lane count, one or four. */
};

/** Validate every phase before any hardware access, including unsupported lanes. */
pf_storage_bus_result_t pf_storage_serial_phases_valid(const pf_storage_serial_phase_t *phases,
                                                            size_t count, uint8_t lanes);
/** Validate serial capability and report quad support; does not initialize hardware. */
pf_storage_bus_result_t pf_storage_serial_valid(const pf_storage_bus_t *transport,
                                                     bool *quad_supported);
/** Validate manual CS and raw byte exchange for caller-controlled transactions. */
bool pf_storage_serial_raw_valid(const pf_storage_bus_t *transport);
/** Execute caller-supplied phases under one CS assertion; no command construction or replay. */
pf_storage_bus_result_t pf_storage_serial_transfer(const pf_storage_bus_t *transport,
                                                        const pf_storage_serial_phase_t *phases, size_t count);
/** Assert/deassert CS for caller-sequenced raw byte exchanges. */
pf_storage_bus_result_t pf_storage_serial_select(const pf_storage_bus_t *transport, bool selected);
/** Exchange bytes without changing CS; NULL TX sends ones, NULL RX discards. */
pf_storage_bus_result_t pf_storage_serial_exchange(const pf_storage_bus_t *transport,
                                                        const uint8_t *tx, uint8_t *rx, size_t length);

#endif
