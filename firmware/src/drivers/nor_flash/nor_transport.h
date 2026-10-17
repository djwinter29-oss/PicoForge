#ifndef PF_NOR_TRANSPORT_H
#define PF_NOR_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../storage/storage_transport.h"

typedef enum {
    PF_NOR_OK = 0,
    PF_NOR_INVALID_ARGUMENT,
    PF_NOR_NOT_READY,
    PF_NOR_UNSUPPORTED,
    PF_NOR_IO_ERROR,
    PF_NOR_TIMEOUT,
    PF_NOR_VERIFY_ERROR,
} pf_nor_result_t;

/** One CS-bounded 1-1-1 or 1-1-4 transaction; buffers live until execute returns. */
typedef pf_storage_serial_transaction_t pf_nor_transaction_t;

/** Caller-owned transport; callbacks and context must outlive the flash device. */
typedef struct {
    void *context;
    pf_nor_result_t (*execute)(void *context, const pf_nor_transaction_t *transaction);
    uint64_t (*time_us)(void *context);
    bool quad_supported;
} pf_nor_transport_t;

/** Initialize a configured SPI/QSPI storage bus and expose the common NOR transport.
 * The bus handle and its concrete context must remain alive and unchanged until all NOR operations finish.
 * Configure physical clock limits and bounded polling before calling. Native SD buses are unsupported.
 * On success, release resources with pf_storage_transport_deinit after completing device operations.
 * Transfer I/O errors and timeouts deinitialize the bus; recovery requires bus and flash reinitialization.
 */
pf_nor_result_t pf_nor_transport_init(pf_storage_transport_t *bus, pf_nor_transport_t *transport);

/** Validate the supported transaction shape before touching CS or any bus pins. */
static inline bool pf_nor_transaction_valid(const pf_nor_transaction_t *transaction)
{
    return pf_storage_serial_transaction_valid(transaction);
}

/** Translate physical results without relying on enum numeric equivalence. */
static inline pf_nor_result_t pf_nor_result_from_storage(pf_storage_transport_result_t result)
{
    switch (result) {
    case PF_STORAGE_TRANSPORT_OK: return PF_NOR_OK;
    case PF_STORAGE_TRANSPORT_INVALID_ARGUMENT: return PF_NOR_INVALID_ARGUMENT;
    case PF_STORAGE_TRANSPORT_NOT_READY: return PF_NOR_NOT_READY;
    case PF_STORAGE_TRANSPORT_UNSUPPORTED: return PF_NOR_UNSUPPORTED;
    case PF_STORAGE_TRANSPORT_TIMEOUT: return PF_NOR_TIMEOUT;
    case PF_STORAGE_TRANSPORT_IO_ERROR: default: return PF_NOR_IO_ERROR;
    }
}

#endif
