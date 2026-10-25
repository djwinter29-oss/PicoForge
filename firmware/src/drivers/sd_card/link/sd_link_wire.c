/** @file SD CMD/DAT wire dispatch; device commands and policy belong to callers. */
#include "sd_link_wire.h"

/** @brief Resolve SD wire capability without exposing concrete hardware state. */
static const pf_sd_link_wire_ops_t *sd_wire_ops(const pf_sd_link_t *bus)
{
    return bus != NULL && bus->context != NULL && bus->ops != NULL &&
           bus->mode == PF_SD_LINK_NATIVE_SD ? bus->ops->sd_wire : NULL;
}

bool pf_sd_link_wire_valid(const pf_sd_link_t *bus)
{
    const pf_sd_link_wire_ops_t *ops = sd_wire_ops(bus);
    return pf_sd_link_valid(bus, PF_SD_LINK_NATIVE_SD) &&
        ops != NULL && ops->state != NULL && ops->hooks != NULL && ops->clocks != NULL && ops->idle != NULL &&
        ops->exchange != NULL && ops->rx_start != NULL && ops->rx_finish != NULL && ops->rx_cancel != NULL &&
        ops->rx_poll != NULL && ops->rx_wait != NULL && ops->rx_progress != NULL &&
        ops->tx_start != NULL && ops->tx_finish != NULL && ops->wait_high != NULL;
}

pf_sd_link_wire_state_t pf_sd_link_wire_state(const pf_sd_link_t *bus)
{
    const pf_sd_link_wire_ops_t *ops = sd_wire_ops(bus);
    return ops != NULL && ops->state != NULL ? ops->state(bus->context) : (pf_sd_link_wire_state_t){0};
}

bool pf_sd_link_wire_hooks(const pf_sd_link_t *bus,
                            pf_sd_link_wire_error_hook_t error, void *context, void (*idle)(void))
{
    const pf_sd_link_wire_ops_t *ops = sd_wire_ops(bus);
    if (ops == NULL || ops->hooks == NULL) {
        return false;
    }
    ops->hooks(bus->context, error, context, idle);
    return true;
}

bool pf_sd_link_wire_clocks(const pf_sd_link_t *bus, uint32_t cycles, uint32_t timeout_us)
{
    const pf_sd_link_wire_ops_t *ops = sd_wire_ops(bus);
    return ops != NULL && ops->clocks != NULL && cycles != 0u && timeout_us != 0u &&
           ops->clocks(bus->context, cycles, timeout_us);
}

bool pf_sd_link_wire_idle(const pf_sd_link_t *bus)
{
    const pf_sd_link_wire_ops_t *ops = sd_wire_ops(bus);
    return ops != NULL && ops->idle != NULL && ops->idle(bus->context);
}

bool pf_sd_link_wire_exchange(const pf_sd_link_t *bus, const uint8_t *tx, size_t tx_size,
                                uint16_t rx_bits, uint8_t *rx, size_t capacity, size_t *received,
                                uint32_t timeout_us)
{
    if (received != NULL) {
        *received = 0u;
    }
    const pf_sd_link_wire_ops_t *ops = sd_wire_ops(bus);
    return ops != NULL && ops->exchange != NULL && tx != NULL && tx_size != 0u &&
           rx_bits % 8u == 0u && (rx_bits == 0u || (rx != NULL && capacity >= rx_bits / 8u)) &&
           received != NULL && timeout_us != 0u &&
           ops->exchange(bus->context, tx, tx_size, rx_bits, rx, capacity, received, timeout_us);
}

bool pf_sd_link_wire_rx_start(const pf_sd_link_t *bus, uint32_t *buffer,
                                uint32_t words, uint32_t nibbles, uint32_t blocks)
{
    const pf_sd_link_wire_ops_t *ops = sd_wire_ops(bus);
    return ops != NULL && ops->rx_start != NULL && buffer != NULL && words != 0u && nibbles != 0u &&
           blocks != 0u && ops->rx_start(bus->context, buffer, words, nibbles, blocks);
}

bool pf_sd_link_wire_rx_finish(const pf_sd_link_t *bus, uint32_t timeout_us)
{
    const pf_sd_link_wire_ops_t *ops = sd_wire_ops(bus);
    return ops != NULL && ops->rx_finish != NULL && timeout_us != 0u &&
           ops->rx_finish(bus->context, timeout_us);
}

void pf_sd_link_wire_rx_cancel(const pf_sd_link_t *bus)
{
    const pf_sd_link_wire_ops_t *ops = sd_wire_ops(bus);
    if (ops != NULL && ops->rx_cancel != NULL) {
        ops->rx_cancel(bus->context);
    }
}

bool pf_sd_link_wire_rx_poll(const pf_sd_link_t *bus, uint32_t start,
                               uint32_t timeout_us, bool *complete)
{
    const pf_sd_link_wire_ops_t *ops = sd_wire_ops(bus);
    return ops != NULL && ops->rx_poll != NULL && complete != NULL && timeout_us != 0u &&
           ops->rx_poll(bus->context, start, timeout_us, complete);
}

void pf_sd_link_wire_rx_wait(const pf_sd_link_t *bus)
{
    const pf_sd_link_wire_ops_t *ops = sd_wire_ops(bus);
    if (ops != NULL && ops->rx_wait != NULL) {
        ops->rx_wait(bus->context);
    }
}

bool pf_sd_link_wire_rx_progress(const pf_sd_link_t *bus,
                                   const uint32_t *buffer, uint32_t capacity, uint32_t *words)
{
    const pf_sd_link_wire_ops_t *ops = sd_wire_ops(bus);
    return ops != NULL && ops->rx_progress != NULL && buffer != NULL && words != NULL && capacity != 0u &&
           ops->rx_progress(bus->context, buffer, capacity, words);
}

bool pf_sd_link_wire_tx_start(const pf_sd_link_t *bus,
                                const uint32_t *buffer, uint32_t words, uint32_t nibbles)
{
    const pf_sd_link_wire_ops_t *ops = sd_wire_ops(bus);
    return ops != NULL && ops->tx_start != NULL && buffer != NULL && words != 0u && nibbles != 0u &&
           ops->tx_start(bus->context, buffer, words, nibbles);
}

bool pf_sd_link_wire_tx_finish(const pf_sd_link_t *bus, uint32_t timeout_us, uint32_t *response)
{
    const pf_sd_link_wire_ops_t *ops = sd_wire_ops(bus);
    return ops != NULL && ops->tx_finish != NULL && timeout_us != 0u && response != NULL &&
           ops->tx_finish(bus->context, timeout_us, response);
}

bool pf_sd_link_wire_wait_high(const pf_sd_link_t *bus, uint32_t timeout_us)
{
    const pf_sd_link_wire_ops_t *ops = sd_wire_ops(bus);
    return ops != NULL && ops->wait_high != NULL && timeout_us != 0u &&
           ops->wait_high(bus->context, timeout_us);
}
