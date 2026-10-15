#include "sd_card_port.h"

void pf_sd_card_port_init(const pf_sd_card_config_t *config)
{
    (void)config;
}

void pf_sd_card_port_lock(void)
{
}

bool pf_sd_card_port_try_lock(void)
{
    return true;
}

void pf_sd_card_port_unlock(void)
{
}
