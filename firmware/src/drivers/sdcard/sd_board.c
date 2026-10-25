/** @file
 * @brief Shared SD setup: saved speed and card detect over a statically linked board transport.
 */
#include "sd_board_internal.h"

#include <stddef.h>
#include <string.h>

#include "drivers/boot_config.h"
#include "hardware/gpio.h"

static bool sd_board_bound; /**< Transport context and card-detect GPIO have been prepared. */

/** @brief Sample the board card-detect input. */
static bool card_present(const void *context)
{
    (void)context;
    bool active_high;
    const uint8_t gpio = pf_sd_board_transport_detect(&active_high);
    return gpio == UINT8_MAX || (gpio_get(gpio) != 0u) == active_high;
}

bool pf_sd_board_config(pf_sd_config_t *config)
{
    const uint32_t speed_hz = pf_get_storage_speed_mhz() * 1000000u;

    if (config == NULL) {
        return false;
    }
    *config = (pf_sd_config_t){
        .transport = pf_sd_board_transport(!sd_board_bound),
        .clock_hz = speed_hz,
    };
    config->native_default_speed_only = config->transport.mode == PF_SD_LINK_NATIVE_SD && speed_hz <= 25000000u;
    if (!sd_board_bound) {
        bool active_high;
        const uint8_t gpio = pf_sd_board_transport_detect(&active_high);
        if (gpio != UINT8_MAX) {
            gpio_init(gpio);
            gpio_disable_pulls(gpio);
        }
        sd_board_bound = true;
    }
    config->transport.present = card_present;
    return true;
}

pf_sd_link_mode_t pf_sd_board_pins(uint8_t bus_gpios[6], uint8_t *detect_gpio)
{
    memset(bus_gpios, UINT8_MAX, 6u);
    bool active_high;
    *detect_gpio = pf_sd_board_transport_detect(&active_high);
    return pf_sd_board_transport_pins(bus_gpios);
}
