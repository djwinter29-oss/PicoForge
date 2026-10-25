/** @file
 * @brief SPI board binding; selected by CMake, never at runtime.
 */
#include "sd_board_internal.h"
#include "drivers/sdcard/spi/spi.h"

static pf_spi_t sd_transport; /**< Persistent physical context borrowed by the SD driver. */

pf_sd_link_t pf_sd_board_transport(bool bind)
{
    if (bind) {
        sd_transport = (pf_spi_t){
            .instance = pf_spi_board_pins.spi_index == 0u ? spi0 : spi1,
            .pins = pf_spi_board_pins.pins,
            .clock_hz = 400000u,
        };
    }
    return pf_spi_bus(&sd_transport);
}

pf_sd_link_mode_t pf_sd_board_transport_pins(uint8_t bus_gpios[6])
{
    bus_gpios[0] = pf_spi_board_pins.pins.clock_gpio;
    bus_gpios[1] = pf_spi_board_pins.pins.mosi_gpio;
    bus_gpios[2] = pf_spi_board_pins.pins.miso_gpio;
    bus_gpios[3] = pf_spi_board_pins.pins.cs_gpio;
    return PF_SD_LINK_SPI;
}

uint8_t pf_sd_board_transport_detect(bool *active_high)
{
    *active_high = pf_spi_board_pins.card_detect_active_high;
    return pf_spi_board_pins.card_detect_gpio;
}
