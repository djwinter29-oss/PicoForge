/** @file
 * @brief Non-owning physical bus dispatch; device protocols remain above this layer.
 */
#include "bus.h"
#include <assert.h>

bool pf_storage_spi_pins_match_controller(uint32_t spi_index, uint32_t clock_gpio,
                                         uint32_t mosi_gpio, uint32_t miso_gpio)
{
    return spi_index <= 1u && clock_gpio % 4u == 2u && mosi_gpio % 4u == 3u &&
           miso_gpio % 4u == 0u && (clock_gpio / 8u) % 2u == spi_index &&
           (mosi_gpio / 8u) % 2u == spi_index && (miso_gpio / 8u) % 2u == spi_index;
}

static bool bus_valid(const pf_storage_bus_t *bus)
{
    return bus != NULL && bus->context != NULL && bus->ops != NULL &&
           bus->mode >= PF_STORAGE_BUS_SPI && bus->mode <= PF_STORAGE_BUS_QSPI;
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

bool pf_storage_bus_valid(const pf_storage_bus_t *bus, pf_storage_bus_mode_t mode)
{
    if (!bus_valid(bus) || bus->mode != mode) {
        return false;
    }
    return bus->ops->init != NULL && bus->ops->deinit != NULL && bus->ops->abort != NULL &&
           bus->ops->set_clock != NULL && bus->ops->time_us != NULL && bus->ops->delay_ms != NULL;
}

bool pf_storage_bus_same(const pf_storage_bus_t *left, const pf_storage_bus_t *right)
{
    return left != NULL && right != NULL && left->context == right->context && left->ops == right->ops &&
           left->mode == right->mode && left->present == right->present &&
           left->presence_context == right->presence_context;
}

bool pf_storage_bus_presence_known(const pf_storage_bus_t *bus)
{
    return bus_valid(bus) && bus->present != NULL;
}

bool pf_storage_bus_present(const pf_storage_bus_t *bus)
{
    return bus_valid(bus) && (bus->present == NULL || bus->present(bus->presence_context));
}

uint64_t pf_storage_bus_time_us(const pf_storage_bus_t *bus)
{
    assert(bus_valid(bus) && bus->ops->time_us != NULL);
    return bus->ops->time_us();
}

void pf_storage_bus_delay_ms(const pf_storage_bus_t *bus, uint32_t duration)
{
    assert(bus_valid(bus) && bus->ops->delay_ms != NULL);
    bus->ops->delay_ms(duration);
}

bool pf_storage_bus_enable_wait(const pf_storage_bus_t *bus)
{
    return bus_valid(bus) && bus->ops->enable_wait != NULL && bus->ops->enable_wait(bus->context);
}
