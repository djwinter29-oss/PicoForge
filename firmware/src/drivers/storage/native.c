/** @file Raw CMD/DAT capability dispatch; device commands and policy belong to callers. */
#include "native.h"

/** @brief Resolve native capability without exposing concrete hardware state. */
static const pf_storage_native_ops_t *native_ops(const pf_storage_transport_t *transport)
{
    return transport != NULL && transport->context != NULL && transport->ops != NULL &&
           transport->mode == PF_STORAGE_TRANSPORT_NATIVE_SD ? transport->ops->native : NULL;
}

bool pf_storage_native_valid(const pf_storage_transport_t *transport)
{
    const pf_storage_native_ops_t *ops = native_ops(transport);
    return pf_storage_transport_valid(transport, PF_STORAGE_TRANSPORT_NATIVE_SD) &&
        ops != NULL && ops->state != NULL && ops->hooks != NULL && ops->clocks != NULL && ops->idle != NULL &&
        ops->exchange != NULL && ops->rx_start != NULL && ops->rx_finish != NULL && ops->rx_cancel != NULL &&
        ops->rx_poll != NULL && ops->rx_wait != NULL && ops->rx_progress != NULL &&
        ops->tx_start != NULL && ops->tx_finish != NULL && ops->wait_high != NULL;
}

pf_storage_native_state_t pf_storage_native_state(const pf_storage_transport_t *transport)
{
    const pf_storage_native_ops_t *ops = native_ops(transport);
    return ops != NULL && ops->state != NULL ? ops->state(transport->context) : (pf_storage_native_state_t){0};
}

bool pf_storage_native_hooks(const pf_storage_transport_t *transport,
                            pf_storage_native_error_hook_t error, void *context, void (*idle)(void))
{
    const pf_storage_native_ops_t *ops = native_ops(transport);
    if (ops == NULL || ops->hooks == NULL) {
        return false;
    }
    ops->hooks(transport->context, error, context, idle);
    return true;
}

bool pf_storage_native_clocks(const pf_storage_transport_t *transport, uint32_t cycles, uint32_t timeout_us)
{
    const pf_storage_native_ops_t *ops = native_ops(transport);
    return ops != NULL && ops->clocks != NULL && cycles != 0u && timeout_us != 0u &&
           ops->clocks(transport->context, cycles, timeout_us);
}

bool pf_storage_native_idle(const pf_storage_transport_t *transport)
{
    const pf_storage_native_ops_t *ops = native_ops(transport);
    return ops != NULL && ops->idle != NULL && ops->idle(transport->context);
}

bool pf_storage_native_exchange(const pf_storage_transport_t *transport, const uint8_t *tx, size_t tx_size,
                                uint16_t rx_bits, uint8_t *rx, size_t capacity, size_t *received,
                                uint32_t timeout_us)
{
    if (received != NULL) {
        *received = 0u;
    }
    const pf_storage_native_ops_t *ops = native_ops(transport);
    return ops != NULL && ops->exchange != NULL && tx != NULL && tx_size != 0u &&
           rx_bits % 8u == 0u && (rx_bits == 0u || (rx != NULL && capacity >= rx_bits / 8u)) &&
           received != NULL && timeout_us != 0u &&
           ops->exchange(transport->context, tx, tx_size, rx_bits, rx, capacity, received, timeout_us);
}

bool pf_storage_native_rx_start(const pf_storage_transport_t *transport, uint32_t *buffer,
                                uint32_t words, uint32_t nibbles, uint32_t blocks)
{
    const pf_storage_native_ops_t *ops = native_ops(transport);
    return ops != NULL && ops->rx_start != NULL && buffer != NULL && words != 0u && nibbles != 0u &&
           blocks != 0u && ops->rx_start(transport->context, buffer, words, nibbles, blocks);
}

bool pf_storage_native_rx_finish(const pf_storage_transport_t *transport, uint32_t timeout_us)
{
    const pf_storage_native_ops_t *ops = native_ops(transport);
    return ops != NULL && ops->rx_finish != NULL && timeout_us != 0u &&
           ops->rx_finish(transport->context, timeout_us);
}

void pf_storage_native_rx_cancel(const pf_storage_transport_t *transport)
{
    const pf_storage_native_ops_t *ops = native_ops(transport);
    if (ops != NULL && ops->rx_cancel != NULL) {
        ops->rx_cancel(transport->context);
    }
}

bool pf_storage_native_rx_poll(const pf_storage_transport_t *transport, uint32_t start,
                               uint32_t timeout_us, bool *complete)
{
    const pf_storage_native_ops_t *ops = native_ops(transport);
    return ops != NULL && ops->rx_poll != NULL && complete != NULL && timeout_us != 0u &&
           ops->rx_poll(transport->context, start, timeout_us, complete);
}

void pf_storage_native_rx_wait(const pf_storage_transport_t *transport)
{
    const pf_storage_native_ops_t *ops = native_ops(transport);
    if (ops != NULL && ops->rx_wait != NULL) {
        ops->rx_wait(transport->context);
    }
}

bool pf_storage_native_rx_progress(const pf_storage_transport_t *transport,
                                   const uint32_t *buffer, uint32_t capacity, uint32_t *words)
{
    const pf_storage_native_ops_t *ops = native_ops(transport);
    return ops != NULL && ops->rx_progress != NULL && buffer != NULL && words != NULL && capacity != 0u &&
           ops->rx_progress(transport->context, buffer, capacity, words);
}

bool pf_storage_native_tx_start(const pf_storage_transport_t *transport,
                                const uint32_t *buffer, uint32_t words, uint32_t nibbles)
{
    const pf_storage_native_ops_t *ops = native_ops(transport);
    return ops != NULL && ops->tx_start != NULL && buffer != NULL && words != 0u && nibbles != 0u &&
           ops->tx_start(transport->context, buffer, words, nibbles);
}

bool pf_storage_native_tx_finish(const pf_storage_transport_t *transport, uint32_t timeout_us, uint32_t *response)
{
    const pf_storage_native_ops_t *ops = native_ops(transport);
    return ops != NULL && ops->tx_finish != NULL && timeout_us != 0u && response != NULL &&
           ops->tx_finish(transport->context, timeout_us, response);
}

bool pf_storage_native_wait_high(const pf_storage_transport_t *transport, uint32_t timeout_us)
{
    const pf_storage_native_ops_t *ops = native_ops(transport);
    return ops != NULL && ops->wait_high != NULL && timeout_us != 0u &&
           ops->wait_high(transport->context, timeout_us);
}
