/** @file NOR API compatibility over the generic single/quad serial transport. */
#include "nor_qspi_pio.h"

#include "pico/stdlib.h"

static uint64_t now(void *context)
{
    (void)context;
    return time_us_64();
}

static pf_nor_result_t execute(void *context, const pf_nor_transaction_t *transaction)
{
    pf_storage_bus_t bus = pf_storage_qspi_pio_bus(context);
    return pf_nor_result_from_storage(pf_storage_bus_transfer(&bus, transaction));
}

pf_nor_result_t pf_nor_qspi_pio_init(pf_nor_qspi_pio_t *context, pf_nor_transport_t *transport)
{
    if (transport == NULL) {
        return PF_NOR_INVALID_ARGUMENT;
    }
    pf_storage_bus_t bus = pf_storage_qspi_pio_bus(context);
    pf_nor_result_t result = pf_nor_result_from_storage(pf_storage_bus_init(&bus));
    if (result == PF_NOR_OK) {
        *transport = (pf_nor_transport_t){
            .context = context, .execute = execute, .time_us = now, .quad_supported = true,
        };
    }
    return result;
}

void pf_nor_qspi_pio_deinit(pf_nor_qspi_pio_t *context)
{
    pf_storage_bus_t bus = pf_storage_qspi_pio_bus(context);
    pf_storage_bus_deinit(&bus);
}
