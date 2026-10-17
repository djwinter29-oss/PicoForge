/** @file
 * @brief Non-owning physical transport dispatch; device protocols remain above this layer.
 */
#include "storage_bus.h"

static bool bus_valid(const pf_storage_bus_t *bus)
{
    return bus != NULL && bus->context != NULL && bus->ops != NULL &&
           bus->mode >= PF_STORAGE_BUS_SPI && bus->mode <= PF_STORAGE_BUS_QSPI_PIO;
}

pf_storage_bus_result_t pf_storage_bus_init(const pf_storage_bus_t *bus)
{
    return bus_valid(bus) && bus->ops->init != NULL
        ? bus->ops->init(bus->context) : PF_STORAGE_BUS_INVALID_ARGUMENT;
}

void pf_storage_bus_deinit(const pf_storage_bus_t *bus)
{
    if (bus_valid(bus) && bus->ops->deinit != NULL) {
        bus->ops->deinit(bus->context);
    }
}

void pf_storage_bus_abort(const pf_storage_bus_t *bus)
{
    if (bus_valid(bus) && bus->ops->abort != NULL) {
        bus->ops->abort(bus->context);
    }
}

pf_storage_bus_result_t pf_storage_bus_set_clock(const pf_storage_bus_t *bus,
                                                uint32_t requested_hz, uint32_t *actual_hz)
{
    if (!bus_valid(bus) || requested_hz == 0u || actual_hz == NULL) {
        return PF_STORAGE_BUS_INVALID_ARGUMENT;
    }
    return bus->ops->set_clock != NULL
        ? bus->ops->set_clock(bus->context, requested_hz, actual_hz) : PF_STORAGE_BUS_UNSUPPORTED;
}

bool pf_storage_serial_transaction_valid(const pf_storage_serial_transaction_t *transaction)
{
    return transaction != NULL && transaction->address_bytes <= 3u &&
           (transaction->address_bytes == 0u ? transaction->address == 0u :
            transaction->address < (1u << (8u * transaction->address_bytes))) &&
           (transaction->data_lanes == 1u || transaction->data_lanes == 4u) &&
           !(transaction->tx != NULL && transaction->rx != NULL) &&
           (transaction->length == 0u || transaction->tx != NULL || transaction->rx != NULL);
}

pf_storage_bus_result_t pf_storage_bus_transfer(const pf_storage_bus_t *bus,
    const pf_storage_serial_transaction_t *transaction)
{
    if (!bus_valid(bus) || !pf_storage_serial_transaction_valid(transaction)) {
        return PF_STORAGE_BUS_INVALID_ARGUMENT;
    }
    if (bus->mode == PF_STORAGE_BUS_SDIO_PIO || bus->ops->transfer == NULL) {
        return PF_STORAGE_BUS_UNSUPPORTED;
    }
    return bus->ops->transfer(bus->context, transaction);
}
