/** @file Raw serial capability dispatch and whole-transaction validation. */
#include "sd_link_serial.h"

/** @brief Resolve serial capability only for a structurally valid serial handle. */
static const pf_sd_link_serial_ops_t *serial_ops(const pf_sd_link_t *transport)
{
    return transport != NULL && transport->context != NULL && transport->ops != NULL &&
           transport->mode == PF_SD_LINK_SPI
        ? transport->ops->serial : NULL;
}

pf_sd_link_result_t pf_sd_link_serial_phases_valid(const pf_sd_link_serial_phase_t *phases,
                                                            size_t count, uint8_t lanes)
{
    if (phases == NULL || count == 0u || lanes != 1u) {
        return PF_SD_LINK_INVALID_ARGUMENT;
    }
    for (size_t index = 0u; index < count; ++index) {
        const pf_sd_link_serial_phase_t *phase = &phases[index];
        if ((phase->lanes != 1u && phase->lanes != 4u) || phase->bit_count == 0u ||
            phase->bit_count % phase->lanes != 0u ||
            ((phase->tx != NULL || phase->rx != NULL) && phase->bit_count % 8u != 0u)) {
            return PF_SD_LINK_INVALID_ARGUMENT;
        }
        if (phase->lanes > lanes) {
            return PF_SD_LINK_UNSUPPORTED;
        }
        if (lanes == 1u && phase->bit_count % 8u != 0u) {
            return PF_SD_LINK_UNSUPPORTED;
        }
    }
    return PF_SD_LINK_OK;
}

bool pf_sd_link_serial_raw_valid(const pf_sd_link_t *transport)
{
    const pf_sd_link_serial_ops_t *ops = serial_ops(transport);
    return pf_sd_link_valid(transport, PF_SD_LINK_SPI) &&
           ops != NULL && ops->select != NULL && ops->exchange != NULL;
}

pf_sd_link_result_t pf_sd_link_serial_transfer(const pf_sd_link_t *transport,
                                                        const pf_sd_link_serial_phase_t *phases, size_t count)
{
    if (transport == NULL || transport->context == NULL || transport->ops == NULL) {
        return PF_SD_LINK_INVALID_ARGUMENT;
    }
    const pf_sd_link_serial_ops_t *ops = serial_ops(transport);
    if (ops == NULL || ops->transfer == NULL) {
        return PF_SD_LINK_UNSUPPORTED;
    }
    pf_sd_link_result_t result = pf_sd_link_serial_phases_valid(phases, count, ops->lanes);
    return result == PF_SD_LINK_OK ? ops->transfer(transport->context, phases, count) : result;
}

pf_sd_link_result_t pf_sd_link_serial_select(const pf_sd_link_t *transport, bool selected)
{
    if (transport == NULL || transport->context == NULL || transport->ops == NULL) {
        return PF_SD_LINK_INVALID_ARGUMENT;
    }
    const pf_sd_link_serial_ops_t *ops = serial_ops(transport);
    return ops != NULL && ops->select != NULL ? ops->select(transport->context, selected) :
                                              PF_SD_LINK_UNSUPPORTED;
}

pf_sd_link_result_t pf_sd_link_serial_exchange(const pf_sd_link_t *transport,
                                                        const uint8_t *tx, uint8_t *rx, size_t length)
{
    if (transport == NULL || transport->context == NULL || transport->ops == NULL ||
        (length != 0u && tx == NULL && rx == NULL)) {
        return PF_SD_LINK_INVALID_ARGUMENT;
    }
    const pf_sd_link_serial_ops_t *ops = serial_ops(transport);
    return ops != NULL && ops->exchange != NULL ? ops->exchange(transport->context, tx, rx, length) :
                                                PF_SD_LINK_UNSUPPORTED;
}
