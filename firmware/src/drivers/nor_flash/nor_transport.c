/** @file
 * @brief Common NOR adapter over caller-owned SPI or QSPI storage buses.
 */
#include "nor_transport.h"
#include "../storage/serial.h"

static uint64_t now(void *context)
{
    return pf_storage_transport_time_us(context);
}

/** @brief Frame one NOR transaction as raw phases; invalidate failed hardware without replay. */
static pf_nor_result_t execute(void *context, const pf_nor_transaction_t *transaction)
{
    pf_storage_transport_t *bus = context;
    if (!pf_nor_transaction_valid(transaction)) {
        return PF_NOR_INVALID_ARGUMENT;
    }
    uint8_t header[4] = {transaction->command};
    for (uint8_t index = 0u; index < transaction->address_bytes; ++index) {
        header[index + 1u] = (uint8_t)(transaction->address >>
            (8u * (transaction->address_bytes - index - 1u)));
    }
    pf_storage_serial_phase_t phases[3] = {
        {.tx = header, .bit_count = (size_t)(transaction->address_bytes + 1u) * 8u, .lanes = 1u},
    };
    size_t count = 1u;
    if (transaction->dummy_cycles != 0u) {
        phases[count++] = (pf_storage_serial_phase_t){
            .bit_count = transaction->dummy_cycles, .lanes = 1u,
        };
    }
    if (transaction->length != 0u) {
        phases[count++] = (pf_storage_serial_phase_t){
            .tx = transaction->tx, .rx = transaction->rx,
            .bit_count = transaction->length * 8u, .lanes = transaction->data_lanes,
        };
    }
    pf_nor_result_t result = pf_nor_result_from_storage(pf_storage_serial_transfer(bus, phases, count));
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
    pf_nor_result_t validation = pf_nor_result_from_storage(pf_storage_serial_valid(bus, &quad));
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
