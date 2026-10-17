#ifndef PF_NOR_FLASH_TRANSPORT_H
#define PF_NOR_FLASH_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../storage/storage_bus.h"

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

/** Validate the supported transaction shape before touching CS or any bus pins. */
static inline bool pf_nor_transaction_valid(const pf_nor_transaction_t *transaction)
{
    return pf_storage_serial_transaction_valid(transaction);
}

/** Translate physical results without relying on enum numeric equivalence. */
static inline pf_nor_result_t pf_nor_result_from_storage(pf_storage_bus_result_t result)
{
    switch (result) {
    case PF_STORAGE_BUS_OK: return PF_NOR_OK;
    case PF_STORAGE_BUS_INVALID_ARGUMENT: return PF_NOR_INVALID_ARGUMENT;
    case PF_STORAGE_BUS_NOT_READY: return PF_NOR_NOT_READY;
    case PF_STORAGE_BUS_UNSUPPORTED: return PF_NOR_UNSUPPORTED;
    case PF_STORAGE_BUS_TIMEOUT: return PF_NOR_TIMEOUT;
    case PF_STORAGE_BUS_IO_ERROR: default: return PF_NOR_IO_ERROR;
    }
}

#endif
