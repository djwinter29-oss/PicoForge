/** @file
 * @brief Serialized facade over the build-selected SD backend; no filesystem or USB ownership policy.
 */
#include "sd.h"
#include "sd_port.h"
#include "sd_backend.h"

#include <stddef.h>

static bool backend_configured; /**< Valid init attempt permits backend dispatch, not proof of lock or readiness. */

bool pf_sd_try_lock(void) { return pf_sd_port_try_lock(); }
void pf_sd_lock(void) { pf_sd_port_lock(); }
void pf_sd_unlock(void) { pf_sd_port_unlock(); }

pf_sd_result_t pf_sd_init(const pf_sd_config_t *config)
{
    pf_sd_lock();
    pf_sd_result_t result = PF_SD_INVALID_ARGUMENT;
    if (config != NULL && config->transport.mode == pf_sd_backend_mode()) {
        result = pf_sd_backend_init(config);
        if (result != PF_SD_INVALID_ARGUMENT) {
            backend_configured = true;
        }
    }
    pf_sd_unlock();
    return result;
}

bool pf_sd_is_initialized(void)
{
    pf_sd_lock();
    bool ready = backend_configured && pf_sd_backend_is_initialized();
    pf_sd_unlock();
    return ready;
}

uint32_t pf_sd_block_count(void)
{
    pf_sd_lock();
    uint32_t count = pf_sd_is_initialized() ? pf_sd_backend_block_count() : 0u;
    pf_sd_unlock();
    return count;
}

pf_sd_result_t pf_sd_read_blocks(uint32_t first, void *buffer, uint32_t count)
{
    pf_sd_lock();
    pf_sd_result_t result = backend_configured ? pf_sd_backend_read_blocks(first, buffer, count) : PF_SD_NOT_CONFIGURED;
    pf_sd_unlock();
    return result;
}

pf_sd_result_t pf_sd_write_blocks(uint32_t first, const void *buffer, uint32_t count)
{
    pf_sd_lock();
    pf_sd_result_t result = backend_configured ? pf_sd_backend_write_blocks(first, buffer, count) :
                                                PF_SD_NOT_CONFIGURED;
    pf_sd_unlock();
    return result;
}

pf_sd_result_t pf_sd_sync(void)
{
    pf_sd_lock();
    pf_sd_result_t result = backend_configured ? pf_sd_backend_sync() : PF_SD_NOT_CONFIGURED;
    pf_sd_unlock();
    return result;
}

void pf_sd_get_info(pf_sd_info_t *info)
{
    pf_sd_lock();
    if (info != NULL) {
        *info = (pf_sd_info_t){0};
        if (backend_configured) {
            pf_sd_backend_get_info(info);
        }
    }
    pf_sd_unlock();
}

void pf_sd_confirm_removed(void)
{
    pf_sd_lock();
    if (backend_configured) {
        pf_sd_backend_confirm_removed();
    }
    pf_sd_unlock();
}
