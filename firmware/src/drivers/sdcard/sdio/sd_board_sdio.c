/** @file
 * @brief Native SDIO-PIO board binding; selected by CMake, never at runtime.
 */
#include "sd_board_internal.h"
#include "drivers/sdcard/sdio/sdio_pio.h"

static pf_sdio_pio_t sd_transport; /**< Persistent physical context borrowed by the SD driver. */

pf_sd_link_t pf_sd_board_transport(bool bind)
{
    if (bind) {
        sd_transport.config = (pf_sdio_pio_config_t){
            .pins = pf_sdio_pio_board.pins,
            .pio = pf_sdio_pio_board.pio_index == 0u ? pio0 : pio1,
        };
    }
    return pf_sdio_pio_bus(&sd_transport);
}

pf_sd_link_mode_t pf_sd_board_transport_pins(uint8_t bus_gpios[6])
{
    bus_gpios[0] = pf_sd_board_pin_byte(pf_sdio_pio_board.pins.clock_gpio);
    bus_gpios[1] = pf_sd_board_pin_byte(pf_sdio_pio_board.pins.command_gpio);
    for (size_t index = 0u; index < 4u; ++index) {
        bus_gpios[index + 2u] = pf_sd_board_pin_byte(pf_sdio_pio_board.pins.data_gpio[index]);
    }
    return PF_SD_LINK_NATIVE_SD;
}
