#include "board_resources.h"

#include <stddef.h>

#include "config/config.h"
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SPI
#include "sd_card_spi_protocol.h"
#endif

static uint32_t topology_gpio_limit(uint32_t gpio_count)
{
    return (gpio_count < 32u) ? gpio_count : 32u;
}

static bool pin_is_valid(uint32_t pin, uint32_t gpio_limit)
{
    return (pin != PF_BOARD_PIN_UNASSIGNED) && (pin < gpio_limit);
}

#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SDIO_PIO
static bool data_pins_are_valid(const uint32_t data_gpio[4], uint32_t gpio_limit,
                                uint32_t *gpio_base)
{
    uint32_t base = data_gpio[0];

    for (uint32_t lane = 0u; lane < 4u; lane++) {
        if (!pin_is_valid(data_gpio[lane], gpio_limit) ||
            (data_gpio[lane] != base + lane)) {
            return false;
        }
    }

    *gpio_base = base;
    return true;
}

static bool pin_is_sdio_data(uint32_t pin, const uint32_t data_gpio[4])
{
    for (uint32_t lane = 0u; lane < 4u; lane++) {
        if (pin == data_gpio[lane]) {
            return true;
        }
    }
    return false;
}
#endif

#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SPI
static bool spi_pins_are_valid(const pf_board_resources_t *topology,
                               uint32_t gpio_limit)
{
    uint32_t pins[4] = {
        topology->spi_pins.clock_gpio,
        topology->spi_pins.mosi_gpio,
        topology->spi_pins.miso_gpio,
        topology->spi_pins.cs_gpio,
    };

    if ((topology->spi_index > 1u) ||
        ((topology->spi_clock_hz != 0u) &&
         ((topology->spi_clock_hz < 400000u) ||
          (topology->spi_clock_hz > 25000000u)))) {
        return false;
    }
    for (uint32_t pin_index = 0u; pin_index < 4u; pin_index++) {
        if (!pin_is_valid(pins[pin_index], gpio_limit)) {
            return false;
        }
        for (uint32_t previous_index = 0u; previous_index < pin_index;
             previous_index++) {
            if (pins[pin_index] == pins[previous_index]) {
                return false;
            }
        }
        if ((topology->monitor_uart_enabled &&
             ((pins[pin_index] == topology->monitor_uart_tx_gpio) ||
              (pins[pin_index] == topology->monitor_uart_rx_gpio))) ||
            (topology->led_enabled &&
             (pins[pin_index] == topology->led_gpio)) ||
            (topology->card_detect_enabled &&
             (pins[pin_index] == topology->card_detect_gpio))) {
            return false;
        }
    }
    if (topology->card_detect_enabled &&
        (!pin_is_valid(topology->card_detect_gpio, gpio_limit) ||
         (topology->monitor_uart_enabled &&
          ((topology->card_detect_gpio == topology->monitor_uart_tx_gpio) ||
           (topology->card_detect_gpio == topology->monitor_uart_rx_gpio))) ||
         (topology->led_enabled &&
          (topology->card_detect_gpio == topology->led_gpio)))) {
        return false;
    }
    return pf_sd_spi_pins_match_controller(topology->spi_index, topology->spi_pins.clock_gpio,
                                           topology->spi_pins.mosi_gpio, topology->spi_pins.miso_gpio);
}
#endif

bool pf_board_resources_validate(const pf_board_resources_t *topology)
{
    uint32_t gpio_limit;
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SDIO_PIO
    uint32_t data_gpio_base;
    uint32_t clock_gpio;
#endif

    if ((topology == NULL) || (topology->gpio_count == 0u)) {
        return false;
    }
    gpio_limit = topology_gpio_limit(topology->gpio_count);

    if (topology->led_enabled && !pin_is_valid(topology->led_gpio, gpio_limit)) {
        return false;
    }
    if (topology->monitor_uart_enabled &&
        ((topology->monitor_uart_index > 1u) ||
         !pin_is_valid(topology->monitor_uart_tx_gpio, gpio_limit) ||
         !pin_is_valid(topology->monitor_uart_rx_gpio, gpio_limit) ||
         (topology->monitor_uart_tx_gpio == topology->monitor_uart_rx_gpio) ||
         (topology->monitor_uart_baud_rate == 0u) ||
         (topology->led_enabled &&
          ((topology->monitor_uart_tx_gpio == topology->led_gpio) ||
           (topology->monitor_uart_rx_gpio == topology->led_gpio))))) {
        return false;
    }
    if (!topology->sd_card_enabled) {
        return true;
    }
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SPI
    return spi_pins_are_valid(topology, gpio_limit);
#else
    if ((topology->pio_index > 1u) ||
        !data_pins_are_valid(topology->native_pins.data_gpio, gpio_limit, &data_gpio_base) ||
        (data_gpio_base < 2u) || (data_gpio_base > 28u) ||
        !pin_is_valid(topology->native_pins.command_gpio, gpio_limit) ||
        !pin_is_valid(topology->native_pins.clock_gpio, gpio_limit) ||
        (topology->native_pins.clock_gpio != ((data_gpio_base + 30u) % 32u)) ||
        ((topology->clock_hz != 0u) &&
         (topology->clock_hz != 25000000u))) {
        return false;
    }

    clock_gpio = topology->native_pins.clock_gpio;
    if ((clock_gpio >= gpio_limit) ||
        (topology->native_pins.command_gpio == clock_gpio) ||
        pin_is_sdio_data(topology->native_pins.command_gpio, topology->native_pins.data_gpio)) {
        return false;
    }

    if (topology->monitor_uart_enabled &&
        ((topology->monitor_uart_tx_gpio == topology->native_pins.command_gpio) ||
         (topology->monitor_uart_rx_gpio == topology->native_pins.command_gpio) ||
         (topology->monitor_uart_tx_gpio == clock_gpio) ||
         (topology->monitor_uart_rx_gpio == clock_gpio) ||
         pin_is_sdio_data(topology->monitor_uart_tx_gpio, topology->native_pins.data_gpio) ||
         pin_is_sdio_data(topology->monitor_uart_rx_gpio, topology->native_pins.data_gpio))) {
        return false;
    }

    if (topology->led_enabled &&
        ((topology->led_gpio == topology->native_pins.command_gpio) ||
         (topology->led_gpio == clock_gpio) ||
         pin_is_sdio_data(topology->led_gpio, topology->native_pins.data_gpio))) {
        return false;
    }

    if (topology->card_detect_enabled &&
        (!pin_is_valid(topology->card_detect_gpio, gpio_limit) ||
         (topology->card_detect_gpio == topology->native_pins.command_gpio) ||
         (topology->card_detect_gpio == clock_gpio) ||
         (topology->monitor_uart_enabled &&
          ((topology->card_detect_gpio == topology->monitor_uart_tx_gpio) ||
           (topology->card_detect_gpio == topology->monitor_uart_rx_gpio))) ||
         (topology->led_enabled && (topology->card_detect_gpio == topology->led_gpio)) ||
         pin_is_sdio_data(topology->card_detect_gpio, topology->native_pins.data_gpio))) {
        return false;
    }

    return true;
#endif
}