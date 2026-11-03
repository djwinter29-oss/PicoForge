/**
 * @file led.c
 * @brief Board LED driver.
 */
#include "led.h"

#include <stddef.h>

#include "hardware/gpio.h"

static bool led_on; /**< Last successfully applied logical LED state. */

/*
 * ponytail: state is unlocked; one writer (the USB task, which also owns the CYW43) is assumed. A single-bool race is
 * harmless on Cortex-M. Upgrade path: guard set/is_on with a lock if a second writer appears.
 */
bool pf_led_init(void)
{
    if (pf_led_board.put != NULL) {
        return true;
    }
    if (pf_led_board.gpio >= NUM_BANK0_GPIOS) {
        return false;
    }

    gpio_init((uint)pf_led_board.gpio);
    gpio_set_dir((uint)pf_led_board.gpio, GPIO_OUT);
    gpio_put((uint)pf_led_board.gpio, pf_led_board.active_high ? 0u : 1u);
    led_on = false;
    return true;
}

bool pf_led_set(bool on)
{
    if (pf_led_board.put != NULL) {
        if (!pf_led_board.put(on)) return false;
    } else {
        if (pf_led_board.gpio >= NUM_BANK0_GPIOS) return false;
        gpio_put((uint)pf_led_board.gpio, (on == pf_led_board.active_high) ? 1u : 0u);
    }
    led_on = on;
    return true;
}

bool pf_led_is_on(void)
{
    return led_on;
}
