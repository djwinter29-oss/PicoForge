/** @file
 * @brief Common NOR adapter over caller-owned SPI or QSPI storage buses.
 */
#include "nor_transport.h"

#include "pico/stdlib.h"

static uint64_t now(void *context)
{
    (void)context;
    return time_us_64();
}

/** @brief Forward one serial transaction and invalidate failed hardware without replay. */
static pf_nor_result_t execute(void *context, const pf_nor_transaction_t *transaction)
{
    pf_storage_bus_t *bus = context;
    pf_nor_result_t result = pf_nor_result_from_storage(pf_storage_bus_transfer(bus, transaction));
    if (result == PF_NOR_IO_ERROR || result == PF_NOR_TIMEOUT) {
        pf_storage_bus_deinit(bus);
    }
    return result;
}

pf_nor_result_t pf_nor_transport_init(pf_storage_bus_t *bus, pf_nor_transport_t *transport)
{
    if (bus == NULL || transport == NULL || bus->context == NULL || bus->ops == NULL) {
        return PF_NOR_INVALID_ARGUMENT;
    }
    if (bus->mode != PF_STORAGE_BUS_SPI && bus->mode != PF_STORAGE_BUS_QSPI_PIO) {
        return PF_NOR_UNSUPPORTED;
    }
    if (bus->ops->init == NULL || bus->ops->transfer == NULL || bus->ops->deinit == NULL) {
        return PF_NOR_INVALID_ARGUMENT;
    }
    pf_nor_result_t result = pf_nor_result_from_storage(pf_storage_bus_init(bus));
    if (result == PF_NOR_OK) {
        *transport = (pf_nor_transport_t){
            .context = bus, .execute = execute, .time_us = now,
            .quad_supported = bus->mode == PF_STORAGE_BUS_QSPI_PIO,
        };
    }
    return result;
}
