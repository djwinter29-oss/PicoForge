/** @file
 * @brief Non-owning physical bus dispatch; device protocols remain above this layer.
 */
#include "sd_link.h"
#include <assert.h>

static bool bus_valid(const pf_sd_link_t *bus)
{
    return bus != NULL && bus->context != NULL && bus->ops != NULL &&
           bus->mode >= PF_SD_LINK_SPI && bus->mode <= PF_SD_LINK_NATIVE_SD;
}

pf_sd_link_result_t pf_sd_link_init(const pf_sd_link_t *bus)
{
    return bus_valid(bus) && bus->ops->init != NULL
        ? bus->ops->init(bus->context) : PF_SD_LINK_INVALID_ARGUMENT;
}

void pf_sd_link_deinit(const pf_sd_link_t *bus)
{
    if (bus_valid(bus) && bus->ops->deinit != NULL) {
        bus->ops->deinit(bus->context);
    }
}

void pf_sd_link_abort(const pf_sd_link_t *bus)
{
    if (bus_valid(bus) && bus->ops->abort != NULL) {
        bus->ops->abort(bus->context);
    }
}

pf_sd_link_result_t pf_sd_link_set_clock(const pf_sd_link_t *bus,
                                                uint32_t requested_hz, uint32_t *actual_hz)
{
    if (!bus_valid(bus) || requested_hz == 0u || actual_hz == NULL) {
        return PF_SD_LINK_INVALID_ARGUMENT;
    }
    return bus->ops->set_clock != NULL
        ? bus->ops->set_clock(bus->context, requested_hz, actual_hz) : PF_SD_LINK_UNSUPPORTED;
}

bool pf_sd_link_valid(const pf_sd_link_t *bus, pf_sd_link_mode_t mode)
{
    if (!bus_valid(bus) || bus->mode != mode) {
        return false;
    }
    return bus->ops->init != NULL && bus->ops->deinit != NULL && bus->ops->abort != NULL &&
           bus->ops->set_clock != NULL && bus->ops->time_us != NULL && bus->ops->delay_ms != NULL;
}

bool pf_sd_link_same(const pf_sd_link_t *left, const pf_sd_link_t *right)
{
    return left != NULL && right != NULL && left->context == right->context && left->ops == right->ops &&
           left->mode == right->mode && left->present == right->present &&
           left->presence_context == right->presence_context;
}

bool pf_sd_link_presence_known(const pf_sd_link_t *bus)
{
    return bus_valid(bus) && bus->present != NULL;
}

bool pf_sd_link_present(const pf_sd_link_t *bus)
{
    return bus_valid(bus) && (bus->present == NULL || bus->present(bus->presence_context));
}

uint64_t pf_sd_link_time_us(const pf_sd_link_t *bus)
{
    assert(bus_valid(bus) && bus->ops->time_us != NULL);
    return bus->ops->time_us();
}

void pf_sd_link_delay_ms(const pf_sd_link_t *bus, uint32_t duration)
{
    assert(bus_valid(bus) && bus->ops->delay_ms != NULL);
    bus->ops->delay_ms(duration);
}

bool pf_sd_link_enable_wait(const pf_sd_link_t *bus)
{
    return bus_valid(bus) && bus->ops->enable_wait != NULL && bus->ops->enable_wait(bus->context);
}
