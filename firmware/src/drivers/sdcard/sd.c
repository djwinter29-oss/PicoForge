/** @file
 * @brief Serialized, build-selected SD protocol dispatch without filesystem or USB ownership policy.
 */
#include "sd.h"
#include "sd_port.h"
#include "config/config.h"

#include <stddef.h>

#if PF_CONFIG_SDCARD_TRANSPORT == PF_CONFIG_SDCARD_TRANSPORT_SPI
#include "sd_spi.h"
#define selected_init pf_sd_spi_init
#define selected_ready pf_sd_spi_is_initialized
#define selected_count pf_sd_spi_block_count
#define selected_read pf_sd_spi_read_blocks
#define selected_write pf_sd_spi_write_blocks
#define selected_sync pf_sd_spi_sync
#define selected_info pf_sd_spi_get_info
#define selected_stage pf_sd_spi_get_init_stage
#define SELECTED_MODE PF_SD_LINK_SPI
#define SELECTED_NAME "spi"
#else
#include "sd_native.h"
#define selected_init pf_sd_native_init
#define selected_ready pf_sd_native_is_initialized
#define selected_count pf_sd_native_block_count
#define selected_read pf_sd_native_read_blocks
#define selected_write pf_sd_native_write_blocks
#define selected_sync pf_sd_native_sync
#define selected_info pf_sd_native_get_info
#define selected_stage pf_sd_native_get_init_stage
#define SELECTED_MODE PF_SD_LINK_NATIVE_SD
#define SELECTED_NAME "sdio"
#endif

static bool transport_selected; /**< A validated configuration has reached the selected protocol. */

bool pf_sd_try_lock(void) { return pf_sd_port_try_lock(); }
void pf_sd_lock(void) { pf_sd_port_lock(); }
void pf_sd_unlock(void) { pf_sd_port_unlock(); }

pf_sd_result_t pf_sd_init(const pf_sd_config_t *config)
{
    pf_sd_lock();
    pf_sd_result_t result = PF_SD_INVALID_ARGUMENT;
    if (config != NULL && config->transport.mode == SELECTED_MODE) {
        result = selected_init(config);
        if (result != PF_SD_INVALID_ARGUMENT) {
            transport_selected = true;
        }
    }
    pf_sd_unlock();
    return result;
}

bool pf_sd_is_initialized(void)
{
    pf_sd_lock();
    bool ready = transport_selected && selected_ready();
    pf_sd_unlock();
    return ready;
}

uint32_t pf_sd_block_count(void)
{
    pf_sd_lock();
    uint32_t count = pf_sd_is_initialized() ? selected_count() : 0u;
    pf_sd_unlock();
    return count;
}

pf_sd_result_t pf_sd_read_blocks(uint32_t first, void *buffer, uint32_t count)
{
    pf_sd_lock();
    pf_sd_result_t result = transport_selected ? selected_read(first, buffer, count) : PF_SD_NOT_CONFIGURED;
    pf_sd_unlock();
    return result;
}

pf_sd_result_t pf_sd_write_blocks(uint32_t first, const void *buffer, uint32_t count)
{
    pf_sd_lock();
    pf_sd_result_t result = transport_selected ? selected_write(first, buffer, count) : PF_SD_NOT_CONFIGURED;
    pf_sd_unlock();
    return result;
}

pf_sd_result_t pf_sd_sync(void)
{
    pf_sd_lock();
    pf_sd_result_t result = transport_selected ? selected_sync() : PF_SD_NOT_CONFIGURED;
    pf_sd_unlock();
    return result;
}

void pf_sd_get_info(pf_sd_info_t *info)
{
    pf_sd_lock();
    if (info != NULL) {
        *info = (pf_sd_info_t){0};
        if (transport_selected) {
            selected_info(info);
        }
    }
    pf_sd_unlock();
}

uint32_t pf_sd_get_init_stage(void)
{
    pf_sd_lock();
    uint32_t stage = transport_selected ? (uint32_t)selected_stage() : 0u;
    pf_sd_unlock();
    return stage;
}

const char *pf_sd_transport_name(void)
{
    pf_sd_lock();
    const char *name = transport_selected ? SELECTED_NAME : "unconfigured";
    pf_sd_unlock();
    return name;
}

void pf_sd_confirm_removed(void)
{
    pf_sd_lock();
#if PF_CONFIG_SDCARD_TRANSPORT != PF_CONFIG_SDCARD_TRANSPORT_SPI
    if (transport_selected) {
        pf_sd_native_confirm_card_removed();
    }
#endif
    pf_sd_unlock();
}
