/** @file
 * @brief Bind optional scheduler waits through the storage transport boundary.
 */
#include "sd_port.h"

bool pf_sd_port_init(const pf_sd_config_t *config)
{
#if PF_CONFIG_SD_CARD_INTERRUPT_WAIT
    return config != NULL && config->transport == PF_SD_TRANSPORT_SDIO_PIO &&
           pf_storage_transport_enable_wait(&config->bus.native.transport);
#else
    (void)config;
    return true;
#endif
}
