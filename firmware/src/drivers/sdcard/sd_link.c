/** @file
 * @brief Non-owning descriptor and board-presence helpers; backend calls are statically linked.
 */
#include "sd_link.h"

bool pf_sd_link_valid(const pf_sd_link_t *bus, pf_sd_link_mode_t mode)
{
    return bus != NULL && bus->context != NULL && bus->mode == mode &&
           (mode == PF_SD_LINK_SPI || mode == PF_SD_LINK_NATIVE_SD);
}

bool pf_sd_link_same(const pf_sd_link_t *left, const pf_sd_link_t *right)
{
    return left != NULL && right != NULL && left->context == right->context &&
           left->mode == right->mode && left->present == right->present &&
           left->presence_context == right->presence_context;
}

bool pf_sd_link_presence_known(const pf_sd_link_t *bus)
{
    return bus != NULL && pf_sd_link_valid(bus, bus->mode) && bus->present != NULL;
}

bool pf_sd_link_present(const pf_sd_link_t *bus)
{
    return bus != NULL && pf_sd_link_valid(bus, bus->mode) &&
           (bus->present == NULL || bus->present(bus->presence_context));
}
