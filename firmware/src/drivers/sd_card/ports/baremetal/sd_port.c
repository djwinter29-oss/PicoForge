#include "sd_port.h"

bool pf_sd_port_init(const pf_sd_config_t *config)
{
    (void)config;
    return true;
}

void pf_sd_port_lock(void)
{
}

bool pf_sd_port_try_lock(void)
{
    return true;
}

void pf_sd_port_unlock(void)
{
}
