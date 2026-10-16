#ifndef PF_NOR_FLASH_TRANSPORT_H
#define PF_NOR_FLASH_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
typedef struct {
    uint8_t command;
    uint8_t address_bytes;
    uint32_t address;
    uint8_t dummy_cycles;
    uint8_t data_lanes;
    const uint8_t *tx;
    uint8_t *rx;
    size_t length;
} pf_nor_transaction_t;

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
    return transaction != NULL && transaction->address_bytes <= 3u &&
           (transaction->address_bytes != 0u || transaction->address == 0u) &&
           transaction->address <= 0xffffffu &&
           (transaction->data_lanes == 1u || transaction->data_lanes == 4u) &&
           !(transaction->tx != NULL && transaction->rx != NULL) &&
           (transaction->length == 0u || transaction->tx != NULL || transaction->rx != NULL);
}

#endif
