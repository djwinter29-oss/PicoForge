/** @file
 * @brief Non-owning physical transport dispatch; device protocols remain above this layer.
 */
#include "storage_transport.h"
#include <assert.h>

static bool bus_valid(const pf_storage_transport_t *bus)
{
    return bus != NULL && bus->context != NULL && bus->ops != NULL &&
           bus->mode >= PF_STORAGE_TRANSPORT_SPI && bus->mode <= PF_STORAGE_TRANSPORT_QSPI_PIO;
}

pf_storage_transport_result_t pf_storage_transport_init(const pf_storage_transport_t *bus)
{
    return bus_valid(bus) && bus->ops->init != NULL
        ? bus->ops->init(bus->context) : PF_STORAGE_TRANSPORT_INVALID_ARGUMENT;
}

void pf_storage_transport_deinit(const pf_storage_transport_t *bus)
{
    if (bus_valid(bus) && bus->ops->deinit != NULL) {
        bus->ops->deinit(bus->context);
    }
}

void pf_storage_transport_abort(const pf_storage_transport_t *bus)
{
    if (bus_valid(bus) && bus->ops->abort != NULL) {
        bus->ops->abort(bus->context);
    }
}

pf_storage_transport_result_t pf_storage_transport_set_clock(const pf_storage_transport_t *bus,
                                                uint32_t requested_hz, uint32_t *actual_hz)
{
    if (!bus_valid(bus) || requested_hz == 0u || actual_hz == NULL) {
        return PF_STORAGE_TRANSPORT_INVALID_ARGUMENT;
    }
    return bus->ops->set_clock != NULL
        ? bus->ops->set_clock(bus->context, requested_hz, actual_hz) : PF_STORAGE_TRANSPORT_UNSUPPORTED;
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

pf_storage_transport_result_t pf_storage_transport_transfer(const pf_storage_transport_t *bus,
    const pf_storage_serial_transaction_t *transaction)
{
    if (!bus_valid(bus) || !pf_storage_serial_transaction_valid(transaction)) {
        return PF_STORAGE_TRANSPORT_INVALID_ARGUMENT;
    }
    if (bus->mode == PF_STORAGE_TRANSPORT_SDIO_PIO || bus->ops->transfer == NULL) {
        return PF_STORAGE_TRANSPORT_UNSUPPORTED;
    }
    return bus->ops->transfer(bus->context, transaction);
}

bool pf_storage_transport_valid(const pf_storage_transport_t *bus, pf_storage_transport_mode_t mode)
{
    if (!bus_valid(bus) || bus->mode != mode) {
        return false;
    }
    const pf_storage_native_ops_t *native = bus->ops->native;
    bool native_valid = native != NULL && native->state != NULL && native->hooks != NULL &&
        native->prepare != NULL && native->restore != NULL && native->command != NULL &&
        native->rx_start != NULL && native->rx_finish != NULL && native->rx_cleanup != NULL &&
        native->rx_poll != NULL && native->rx_wait != NULL && native->rx_progress != NULL &&
        native->tx_prepare != NULL && native->tx_start != NULL && native->tx_finish != NULL &&
        native->wait_ready != NULL;
    return bus->ops->init != NULL &&
           bus->ops->deinit != NULL && bus->ops->abort != NULL && bus->ops->set_clock != NULL &&
           bus->ops->time_us != NULL && bus->ops->delay_ms != NULL &&
           (mode == PF_STORAGE_TRANSPORT_SDIO_PIO ? native_valid :
            mode == PF_STORAGE_TRANSPORT_SPI ? bus->ops->select != NULL && bus->ops->exchange != NULL :
            bus->ops->transfer != NULL);
}

bool pf_storage_transport_same(const pf_storage_transport_t *left, const pf_storage_transport_t *right)
{
    return left != NULL && right != NULL && left->context == right->context && left->ops == right->ops &&
           left->mode == right->mode && left->present == right->present &&
           left->presence_context == right->presence_context;
}

pf_storage_transport_result_t pf_storage_transport_validate_serial(const pf_storage_transport_t *bus,
    bool *quad_supported)
{
    if (!bus_valid(bus) || quad_supported == NULL) {
        return PF_STORAGE_TRANSPORT_INVALID_ARGUMENT;
    }
    if (bus->mode == PF_STORAGE_TRANSPORT_SDIO_PIO) {
        return PF_STORAGE_TRANSPORT_UNSUPPORTED;
    }
    if (bus->ops->init == NULL || bus->ops->deinit == NULL || bus->ops->transfer == NULL ||
        bus->ops->time_us == NULL) {
        return PF_STORAGE_TRANSPORT_INVALID_ARGUMENT;
    }
    *quad_supported = bus->mode == PF_STORAGE_TRANSPORT_QSPI_PIO;
    return PF_STORAGE_TRANSPORT_OK;
}

bool pf_storage_transport_presence_known(const pf_storage_transport_t *bus)
{
    return bus_valid(bus) && bus->present != NULL;
}

bool pf_storage_transport_present(const pf_storage_transport_t *bus)
{
    return bus_valid(bus) && (bus->present == NULL || bus->present(bus->presence_context));
}

uint64_t pf_storage_transport_time_us(const pf_storage_transport_t *bus)
{
    assert(bus_valid(bus) && bus->ops->time_us != NULL);
    return bus->ops->time_us();
}

void pf_storage_transport_delay_ms(const pf_storage_transport_t *bus, uint32_t duration)
{
    assert(bus_valid(bus) && bus->ops->delay_ms != NULL);
    bus->ops->delay_ms(duration);
}

bool pf_storage_transport_enable_wait(const pf_storage_transport_t *bus)
{
    return bus_valid(bus) && bus->ops->enable_wait != NULL && bus->ops->enable_wait(bus->context);
}

pf_storage_transport_result_t pf_storage_transport_select(const pf_storage_transport_t *bus, bool selected)
{
    if (!bus_valid(bus)) {
        return PF_STORAGE_TRANSPORT_INVALID_ARGUMENT;
    }
    return bus->ops->select != NULL ? bus->ops->select(bus->context, selected) : PF_STORAGE_TRANSPORT_UNSUPPORTED;
}

pf_storage_transport_result_t pf_storage_transport_exchange(const pf_storage_transport_t *bus,
    const uint8_t *tx, uint8_t *rx, size_t length)
{
    if (!bus_valid(bus) || (length != 0u && tx == NULL && rx == NULL)) {
        return PF_STORAGE_TRANSPORT_INVALID_ARGUMENT;
    }
    return bus->ops->exchange != NULL ? bus->ops->exchange(bus->context, tx, rx, length) :
                                      PF_STORAGE_TRANSPORT_UNSUPPORTED;
}

static const pf_storage_native_ops_t *native_ops(const pf_storage_transport_t *bus)
{
    return bus_valid(bus) && bus->mode == PF_STORAGE_TRANSPORT_SDIO_PIO ? bus->ops->native : NULL;
}

pf_storage_native_state_t pf_storage_transport_native_state(const pf_storage_transport_t *bus)
{
    const pf_storage_native_ops_t *ops = native_ops(bus);
    return ops != NULL && ops->state != NULL ? ops->state(bus->context) : (pf_storage_native_state_t){0};
}

bool pf_storage_transport_native_hooks(const pf_storage_transport_t *bus,
    pf_storage_sdio_error_hook_t error, void *context, void (*idle)(void))
{
    const pf_storage_native_ops_t *ops = native_ops(bus);
    if (ops == NULL || ops->hooks == NULL) {
        return false;
    }
    ops->hooks(bus->context, error, context, idle);
    return true;
}

bool pf_storage_transport_native_prepare(const pf_storage_transport_t *bus)
{
    const pf_storage_native_ops_t *ops = native_ops(bus);
    return ops != NULL && ops->prepare != NULL && ops->prepare(bus->context);
}

bool pf_storage_transport_native_restore(const pf_storage_transport_t *bus)
{
    const pf_storage_native_ops_t *ops = native_ops(bus);
    return ops != NULL && ops->restore != NULL && ops->restore(bus->context);
}

bool pf_storage_transport_native_command(const pf_storage_transport_t *bus, const uint8_t frame[6],
    uint16_t bits, uint32_t *words, size_t capacity)
{
    const pf_storage_native_ops_t *ops = native_ops(bus);
    return ops != NULL && ops->command != NULL && frame != NULL &&
           (bits == 0u || (words != NULL && capacity >= (bits + 31u) / 32u)) &&
           ops->command(bus->context, frame, bits, words, capacity);
}

bool pf_storage_transport_native_rx_start(const pf_storage_transport_t *bus, uint32_t *buffer,
    uint32_t words, uint32_t nibbles, uint32_t blocks)
{
    const pf_storage_native_ops_t *ops = native_ops(bus);
    return ops != NULL && ops->rx_start != NULL && buffer != NULL && words != 0u && nibbles != 0u &&
           blocks != 0u && ops->rx_start(bus->context, buffer, words, nibbles, blocks);
}

bool pf_storage_transport_native_rx_finish(const pf_storage_transport_t *bus, uint32_t timeout_us)
{
    const pf_storage_native_ops_t *ops = native_ops(bus);
    return ops != NULL && ops->rx_finish != NULL && timeout_us != 0u &&
           ops->rx_finish(bus->context, timeout_us);
}

void pf_storage_transport_native_rx_cleanup(const pf_storage_transport_t *bus)
{
    const pf_storage_native_ops_t *ops = native_ops(bus);
    if (ops != NULL && ops->rx_cleanup != NULL) {
        ops->rx_cleanup(bus->context);
    }
}

bool pf_storage_transport_native_rx_poll(const pf_storage_transport_t *bus, uint32_t start,
    uint32_t timeout_us, bool *complete)
{
    const pf_storage_native_ops_t *ops = native_ops(bus);
    return ops != NULL && ops->rx_poll != NULL && complete != NULL && timeout_us != 0u &&
           ops->rx_poll(bus->context, start, timeout_us, complete);
}

void pf_storage_transport_native_rx_wait(const pf_storage_transport_t *bus)
{
    const pf_storage_native_ops_t *ops = native_ops(bus);
    if (ops != NULL && ops->rx_wait != NULL) {
        ops->rx_wait(bus->context);
    }
}

bool pf_storage_transport_native_rx_progress(const pf_storage_transport_t *bus,
    const uint32_t *buffer, uint32_t capacity, uint32_t *words)
{
    const pf_storage_native_ops_t *ops = native_ops(bus);
    return ops != NULL && ops->rx_progress != NULL && buffer != NULL && words != NULL && capacity != 0u &&
           ops->rx_progress(bus->context, buffer, capacity, words);
}

bool pf_storage_transport_native_tx_prepare(const pf_storage_transport_t *bus)
{
    const pf_storage_native_ops_t *ops = native_ops(bus);
    return ops != NULL && ops->tx_prepare != NULL && ops->tx_prepare(bus->context);
}

bool pf_storage_transport_native_tx_start(const pf_storage_transport_t *bus,
    const uint32_t *buffer, uint32_t words, uint32_t nibbles)
{
    const pf_storage_native_ops_t *ops = native_ops(bus);
    return ops != NULL && ops->tx_start != NULL && buffer != NULL && words != 0u && nibbles != 0u &&
           ops->tx_start(bus->context, buffer, words, nibbles);
}

bool pf_storage_transport_native_tx_finish(const pf_storage_transport_t *bus,
    uint32_t timeout_us, uint32_t *response)
{
    const pf_storage_native_ops_t *ops = native_ops(bus);
    return ops != NULL && ops->tx_finish != NULL && response != NULL && timeout_us != 0u &&
           ops->tx_finish(bus->context, timeout_us, response);
}

bool pf_storage_transport_native_wait_ready(const pf_storage_transport_t *bus, uint32_t timeout_us)
{
    const pf_storage_native_ops_t *ops = native_ops(bus);
    return ops != NULL && ops->wait_ready != NULL && timeout_us != 0u &&
           ops->wait_ready(bus->context, timeout_us);
}
