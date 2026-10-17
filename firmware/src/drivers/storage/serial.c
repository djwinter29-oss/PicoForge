/** @file Raw serial capability dispatch and whole-transaction validation. */
#include "serial.h"

/** @brief Resolve serial capability only for a structurally valid serial handle. */
static const pf_storage_serial_ops_t *serial_ops(const pf_storage_bus_t *transport)
{
    return transport != NULL && transport->context != NULL && transport->ops != NULL &&
           (transport->mode == PF_STORAGE_BUS_SPI || transport->mode == PF_STORAGE_BUS_QSPI)
        ? transport->ops->serial : NULL;
}

pf_storage_bus_result_t pf_storage_serial_phases_valid(const pf_storage_serial_phase_t *phases,
                                                            size_t count, uint8_t lanes)
{
    if (phases == NULL || count == 0u || (lanes != 1u && lanes != 4u)) {
        return PF_STORAGE_BUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0u; index < count; ++index) {
        const pf_storage_serial_phase_t *phase = &phases[index];
        if ((phase->lanes != 1u && phase->lanes != 4u) || phase->bit_count == 0u ||
            phase->bit_count % phase->lanes != 0u ||
            ((phase->tx != NULL || phase->rx != NULL) && phase->bit_count % 8u != 0u) ||
            (phase->lanes == 4u && phase->tx != NULL && phase->rx != NULL)) {
            return PF_STORAGE_BUS_INVALID_ARGUMENT;
        }
        if (phase->lanes > lanes) {
            return PF_STORAGE_BUS_UNSUPPORTED;
        }
        if (lanes == 1u && phase->bit_count % 8u != 0u) {
            return PF_STORAGE_BUS_UNSUPPORTED;
        }
    }
    return PF_STORAGE_BUS_OK;
}

pf_storage_bus_result_t pf_storage_serial_valid(const pf_storage_bus_t *transport, bool *quad_supported)
{
    if (transport == NULL || transport->context == NULL || transport->ops == NULL || quad_supported == NULL) {
        return PF_STORAGE_BUS_INVALID_ARGUMENT;
    }
    if (transport->mode == PF_STORAGE_BUS_NATIVE_SD) {
        return PF_STORAGE_BUS_UNSUPPORTED;
    }
    const pf_storage_serial_ops_t *ops = serial_ops(transport);
    if (ops == NULL || ops->transfer == NULL || (ops->lanes != 1u && ops->lanes != 4u) ||
        !pf_storage_bus_valid(transport, transport->mode)) {
        return PF_STORAGE_BUS_INVALID_ARGUMENT;
    }
    *quad_supported = ops->lanes == 4u;
    return PF_STORAGE_BUS_OK;
}

bool pf_storage_serial_raw_valid(const pf_storage_bus_t *transport)
{
    const pf_storage_serial_ops_t *ops = serial_ops(transport);
    return pf_storage_bus_valid(transport, PF_STORAGE_BUS_SPI) &&
           ops != NULL && ops->select != NULL && ops->exchange != NULL;
}

pf_storage_bus_result_t pf_storage_serial_transfer(const pf_storage_bus_t *transport,
                                                        const pf_storage_serial_phase_t *phases, size_t count)
{
    if (transport == NULL || transport->context == NULL || transport->ops == NULL) {
        return PF_STORAGE_BUS_INVALID_ARGUMENT;
    }
    const pf_storage_serial_ops_t *ops = serial_ops(transport);
    if (ops == NULL || ops->transfer == NULL) {
        return PF_STORAGE_BUS_UNSUPPORTED;
    }
    pf_storage_bus_result_t result = pf_storage_serial_phases_valid(phases, count, ops->lanes);
    return result == PF_STORAGE_BUS_OK ? ops->transfer(transport->context, phases, count) : result;
}

pf_storage_bus_result_t pf_storage_serial_select(const pf_storage_bus_t *transport, bool selected)
{
    if (transport == NULL || transport->context == NULL || transport->ops == NULL) {
        return PF_STORAGE_BUS_INVALID_ARGUMENT;
    }
    const pf_storage_serial_ops_t *ops = serial_ops(transport);
    return ops != NULL && ops->select != NULL ? ops->select(transport->context, selected) :
                                              PF_STORAGE_BUS_UNSUPPORTED;
}

pf_storage_bus_result_t pf_storage_serial_exchange(const pf_storage_bus_t *transport,
                                                        const uint8_t *tx, uint8_t *rx, size_t length)
{
    if (transport == NULL || transport->context == NULL || transport->ops == NULL ||
        (length != 0u && tx == NULL && rx == NULL)) {
        return PF_STORAGE_BUS_INVALID_ARGUMENT;
    }
    const pf_storage_serial_ops_t *ops = serial_ops(transport);
    return ops != NULL && ops->exchange != NULL ? ops->exchange(transport->context, tx, rx, length) :
                                                PF_STORAGE_BUS_UNSUPPORTED;
}
