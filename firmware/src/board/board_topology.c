#include "board_topology.h"

#include <stddef.h>

static uint32_t topology_gpio_limit(uint32_t gpio_count)
{
    return (gpio_count < 32u) ? gpio_count : 32u;
}

static bool pin_is_valid(uint32_t pin, uint32_t gpio_limit)
{
    return (pin != PF_BOARD_PIN_UNASSIGNED) && (pin < gpio_limit);
}

bool pf_board_topology_validate(const pf_board_topology_t *topology)
{
    uint32_t gpio_limit;
    uint32_t data3_gpio;
    uint32_t clock_gpio;

    if ((topology == NULL) || (topology->gpio_count == 0u)) {
        return false;
    }
    gpio_limit = topology_gpio_limit(topology->gpio_count);
    if (topology->led_enabled && !pin_is_valid(topology->led_gpio, gpio_limit)) {
        return false;
    }
    if (!topology->sdio_enabled) {
        return true;
    }

    if ((topology->pio_index > 1u) ||
        (topology->data0_gpio < 2u) || (topology->data0_gpio > 28u) ||
        !pin_is_valid(topology->command_gpio, gpio_limit) ||
        ((topology->clock_hz != 0u) &&
         ((topology->clock_hz < 400000u) || (topology->clock_hz > 25000000u)))) {
        return false;
    }

    data3_gpio = topology->data0_gpio + 3u;
    clock_gpio = (topology->data0_gpio + 30u) % 32u;
    if ((data3_gpio >= gpio_limit) || (clock_gpio >= gpio_limit) ||
        (topology->command_gpio == clock_gpio) ||
        ((topology->command_gpio >= topology->data0_gpio) &&
         (topology->command_gpio <= data3_gpio))) {
        return false;
    }

    if (topology->led_enabled &&
        ((topology->led_gpio == topology->command_gpio) ||
         (topology->led_gpio == clock_gpio) ||
         ((topology->led_gpio >= topology->data0_gpio) &&
          (topology->led_gpio <= data3_gpio)))) {
        return false;
    }

    if (topology->card_detect_enabled &&
        (!pin_is_valid(topology->card_detect_gpio, gpio_limit) ||
         (topology->card_detect_gpio == topology->command_gpio) ||
         (topology->card_detect_gpio == clock_gpio) ||
              (topology->led_enabled && (topology->card_detect_gpio == topology->led_gpio)) ||
         ((topology->card_detect_gpio >= topology->data0_gpio) &&
          (topology->card_detect_gpio <= data3_gpio)))) {
        return false;
    }

    return true;
}