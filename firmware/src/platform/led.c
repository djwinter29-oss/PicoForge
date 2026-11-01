/**
 * @file led.c
 * @brief Board LED driver.
 */
#include "led.h"

#include <stddef.h>

#include "hardware/gpio.h"

static bool led_available;
static bool led_on;
static void (*attached_put)(bool on); /**< Non-GPIO LED driver from pf_led_attach, or NULL for the GPIO LED. */

/*
 * ponytail: state is unlocked; one writer (the USB task, which also owns the CYW43) is assumed. A single-bool race is
 * harmless on Cortex-M. Upgrade path: guard set/is_on with a lock if a second writer appears.
 */
bool pf_led_init(void)
{
    if (led_available || pf_led_board.gpio == PF_LED_GPIO_NONE) {
        return true;
    }
    if (pf_led_board.gpio >= NUM_BANK0_GPIOS) {
        return false;
    }

    gpio_init((uint)pf_led_board.gpio);
    gpio_set_dir((uint)pf_led_board.gpio, GPIO_OUT);
    gpio_put((uint)pf_led_board.gpio, pf_led_board.active_high ? 0u : 1u);
    led_on = false;
    led_available = true;
    return true;
}

void pf_led_attach(void (*put)(bool on))
{
    if (led_available || put == NULL) {
        return;
    }
    put(false);
    attached_put = put;
    led_on = false;
    led_available = true;
}

bool pf_led_is_available(void)
{
    return led_available;
}

bool pf_led_set(bool on)
{
    if (!led_available) {
        return false;
    }

    led_on = on;
    if (attached_put != NULL) {
        attached_put(on);
    } else {
        gpio_put((uint)pf_led_board.gpio, (on == pf_led_board.active_high) ? 1u : 0u);
    }
    return true;
}

bool pf_led_is_on(void)
{
    return led_available && led_on;
}
