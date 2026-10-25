/** @file
 * @brief Board-wired SD setup: build-selected transport context, boot-config clock and card detect.
 */
#include "sd_board.h"

#include <stddef.h>

#include "config/config.h"
#include "drivers/boot_config.h"
#include "hardware/gpio.h"
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SDIO_PIO
#include "drivers/sd_card/sdio_pio/sdio_pio.h"
static pf_sdio_pio_t sd_transport; /**< Selected physical context; outlives all SD access. */
#else
#include "drivers/sd_card/spi/spi.h"
static pf_spi_t sd_transport; /**< Selected physical context; outlives all SD access. */
#endif

static bool sd_board_bound; /**< Transport context and card-detect GPIO have been prepared. */

/** @brief Sample the board card-detect input. */
static bool card_present(const void *context)
{
    (void)context;
    return (gpio_get(pf_sd_board.card_detect_gpio) != 0u) == pf_sd_board.card_detect_active_high;
}

bool pf_sd_board_config(pf_sd_config_t *config)
{
    const bool high_speed = pf_boot_config_sd_speed() == PF_SD_SPEED_HIGH;

    if (config == NULL) {
        return false;
    }
    if (!sd_board_bound) {
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SDIO_PIO
        sd_transport.config = (pf_sdio_pio_config_t){
            .pins = pf_sdio_pio_board.pins,
            .pio = pf_sdio_pio_board.pio_index == 0u ? pio0 : pio1,
        };
#else
        sd_transport = (pf_spi_t){
            .instance = pf_spi_board.spi_index == 0u ? spi0 : spi1,
            .pins = pf_spi_board.pins,
            .clock_hz = 400000u,
        };
#endif
        gpio_init(pf_sd_board.card_detect_gpio);
        gpio_disable_pulls(pf_sd_board.card_detect_gpio);
        sd_board_bound = true;
    }
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SDIO_PIO
    *config = (pf_sd_config_t){
        .transport = pf_sdio_pio_bus(&sd_transport),
        .clock_hz = pf_sd_board.clock_hz,
        .native_default_speed_only = !high_speed,
    };
#else
    const uint32_t speed_hz = high_speed ? 50000000u : 25000000u;
    *config = (pf_sd_config_t){
        .transport = pf_spi_bus(&sd_transport),
        .clock_hz = speed_hz < pf_sd_board.clock_hz ? speed_hz : pf_sd_board.clock_hz,
    };
#endif
    config->transport.present = card_present;
    return true;
}
