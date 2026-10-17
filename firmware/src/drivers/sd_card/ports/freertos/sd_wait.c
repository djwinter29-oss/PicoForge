/** @file
 * @brief Bind optional scheduler waits through the storage transport boundary.
 */
#include "sd_port.h"

#if PF_CONFIG_SD_CARD_INTERRUPT_WAIT
#include <assert.h>
#endif

void pf_sd_port_init(const pf_sd_config_t *config)
{
#if PF_CONFIG_SD_CARD_INTERRUPT_WAIT
    if (config != NULL && config->transport == PF_SD_TRANSPORT_SDIO_PIO) {
        bool enabled = pf_storage_transport_enable_wait(&config->bus.native.transport);
        assert(enabled);
        (void)enabled;
    }
#else
    (void)config;
#endif
}
