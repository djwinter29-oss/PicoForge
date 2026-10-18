/** @file
 * @brief Runtime serial-storage resources configured from the immutable board profile.
 */
#include "board_storage.h"
#include "board_resources.h"
#include "hardware/gpio.h"

#if PF_BOARD_STORAGE_QSPI
#include "drivers/storage/qspi_pio/qspi_pio.h"
static pf_qspi_pio_t serial_storage;
#else
#include "drivers/storage/spi/spi.h"
static pf_spi_t serial_storage;
#endif
static bool serial_pins_prepared;

pf_storage_bus_result_t pf_board_storage_prepare(pf_storage_bus_t *bus)
{
    if (bus == NULL) {
        return PF_STORAGE_BUS_INVALID_ARGUMENT;
    }
    if (serial_pins_prepared || serial_storage.initialized) {
        return PF_STORAGE_BUS_NOT_READY;
    }
    const pf_board_resources_t *profile = pf_board_resources_get();
    if (!pf_board_resources_validate_serial(profile, PF_BOARD_STORAGE_QSPI)) {
        return PF_STORAGE_BUS_INVALID_ARGUMENT;
    }
#if PF_BOARD_STORAGE_QSPI
    serial_storage = (pf_qspi_pio_t){
        .pio = profile->pio_index == 0u ? pio0 : pio1,
        .pins = profile->qspi_pins, .clock_hz = profile->serial_clock_hz,
    };
    *bus = pf_qspi_pio_bus(&serial_storage);
#else
    serial_storage = (pf_spi_t){
        .instance = profile->spi_index == 0u ? spi0 : spi1,
        .pins = profile->spi_pins, .clock_hz = profile->serial_clock_hz,
        .timeout_us = profile->serial_timeout_us,
    };
    if (!pf_spi_config_valid(&serial_storage)) {
        return PF_STORAGE_BUS_INVALID_ARGUMENT;
    }
    *bus = pf_spi_bus(&serial_storage);
    gpio_init(profile->serial_wp_gpio);
    gpio_put(profile->serial_wp_gpio, true);
    gpio_set_dir(profile->serial_wp_gpio, GPIO_OUT);
    gpio_init(profile->serial_hold_gpio);
    gpio_put(profile->serial_hold_gpio, true);
    gpio_set_dir(profile->serial_hold_gpio, GPIO_OUT);
#endif
    serial_pins_prepared = true;
    return PF_STORAGE_BUS_OK;
}

void pf_board_storage_release_pins(void)
{
    if (!serial_pins_prepared || serial_storage.initialized) {
        return;
    }
#if !PF_BOARD_STORAGE_QSPI
    const pf_board_resources_t *profile = pf_board_resources_get();
    gpio_init(profile->serial_wp_gpio);
    gpio_init(profile->serial_hold_gpio);
#endif
    serial_pins_prepared = false;
}
