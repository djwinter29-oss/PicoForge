/** @file
 * @brief Common NOR adapter over caller-owned SPI or QSPI storage buses.
 */
#include "nor_transport.h"

static uint64_t now(void *context)
{
    return pf_storage_transport_time_us(context);
}

/** @brief Forward one serial transaction and invalidate failed hardware without replay. */
static pf_nor_result_t execute(void *context, const pf_nor_transaction_t *transaction)
{
    pf_storage_transport_t *bus = context;
    pf_nor_result_t result = pf_nor_result_from_storage(pf_storage_transport_transfer(bus, transaction));
    if (result == PF_NOR_IO_ERROR || result == PF_NOR_TIMEOUT) {
        pf_storage_transport_deinit(bus);
    }
    return result;
}

pf_nor_result_t pf_nor_transport_init(pf_storage_transport_t *bus, pf_nor_transport_t *transport)
{
    if (transport == NULL || bus == NULL) {
        return PF_NOR_INVALID_ARGUMENT;
    }
    bool quad;
    pf_nor_result_t validation = pf_nor_result_from_storage(pf_storage_transport_validate_serial(bus, &quad));
    if (validation != PF_NOR_OK) {
        return validation;
    }
    pf_nor_result_t result = pf_nor_result_from_storage(pf_storage_transport_init(bus));
    if (result == PF_NOR_OK) {
        *transport = (pf_nor_transport_t){
            .context = bus, .execute = execute, .time_us = now,
            .quad_supported = quad,
        };
    }
    return result;
}
