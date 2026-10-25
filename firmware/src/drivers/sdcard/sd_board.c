/** @file
 * @brief Board-wired SD setup: build-selected transport context, boot-config clock and card detect.
 */
#include "sd_board.h"

#include <stddef.h>
#include <string.h>

#include "config/config.h"
#include "drivers/boot_config.h"
#include "hardware/gpio.h"

/** @brief Narrow a GPIO number to a status byte; out-of-range reads as UINT8_MAX. */
static uint8_t pin_byte(uint32_t gpio)
{
    return gpio > UINT8_MAX - 1u ? UINT8_MAX : (uint8_t)gpio;
}

#if PF_CONFIG_SDCARD_TRANSPORT == PF_CONFIG_SDCARD_TRANSPORT_SDIO_PIO
#include "drivers/sdcard/sdio_pio/sdio_pio.h"
static pf_sdio_pio_t sd_transport; /**< Selected physical context; outlives all SD access. */

/** @brief Bind the selected context on first use and return its borrowed link. */
static pf_sd_link_t board_transport(bool bind)
{
    if (bind) {
        sd_transport.config = (pf_sdio_pio_config_t){
            .pins = pf_sdio_pio_board.pins,
            .pio = pf_sdio_pio_board.pio_index == 0u ? pio0 : pio1,
        };
    }
    return pf_sdio_pio_bus(&sd_transport);
}

/** @brief Copy the selected bus pins into the status layout. */
static pf_sd_link_mode_t board_pins(uint8_t bus_gpios[6])
{
    bus_gpios[0] = pin_byte(pf_sdio_pio_board.pins.clock_gpio);
    bus_gpios[1] = pin_byte(pf_sdio_pio_board.pins.command_gpio);
    for (size_t index = 0u; index < 4u; ++index) {
        bus_gpios[index + 2u] = pin_byte(pf_sdio_pio_board.pins.data_gpio[index]);
    }
    return PF_SD_LINK_NATIVE_SD;
}
#else
#include "drivers/sdcard/spi/spi.h"
static pf_spi_t sd_transport; /**< Selected physical context; outlives all SD access. */

/** @brief Bind the selected context on first use and return its borrowed link. */
static pf_sd_link_t board_transport(bool bind)
{
    if (bind) {
        sd_transport = (pf_spi_t){
            .instance = pf_spi_board.spi_index == 0u ? spi0 : spi1,
            .pins = pf_spi_board.pins,
            .clock_hz = 400000u,
        };
    }
    return pf_spi_bus(&sd_transport);
}

/** @brief Copy the selected bus pins into the status layout. */
static pf_sd_link_mode_t board_pins(uint8_t bus_gpios[6])
{
    bus_gpios[0] = pin_byte(pf_spi_board.pins.clock_gpio);
    bus_gpios[1] = pin_byte(pf_spi_board.pins.mosi_gpio);
    bus_gpios[2] = pin_byte(pf_spi_board.pins.miso_gpio);
    bus_gpios[3] = pin_byte(pf_spi_board.pins.cs_gpio);
    return PF_SD_LINK_SPI;
}
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
    const uint32_t speed_hz = pf_storage_speed_mhz(pf_boot_config_storage_speed()) * 1000000u;

    if (config == NULL) {
        return false;
    }
    *config = (pf_sd_config_t){
        .transport = board_transport(!sd_board_bound),
        .clock_hz = speed_hz,
    };
    config->native_default_speed_only = config->transport.mode == PF_SD_LINK_NATIVE_SD && speed_hz <= 25000000u;
    if (!sd_board_bound) {
        gpio_init(pf_sd_board.card_detect_gpio);
        gpio_disable_pulls(pf_sd_board.card_detect_gpio);
        sd_board_bound = true;
    }
    config->transport.present = card_present;
    return true;
}

pf_sd_link_mode_t pf_sd_board_pins(uint8_t bus_gpios[6], uint8_t *detect_gpio)
{
    memset(bus_gpios, UINT8_MAX, 6u);
    *detect_gpio = pin_byte(pf_sd_board.card_detect_gpio);
    return board_pins(bus_gpios);
}
