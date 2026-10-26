/**
 * @file led.c
 * @brief Board LED driver.
 */
#include "led.h"

#include "hardware/gpio.h"

static bool led_available;
static bool led_on;

/*
 * ponytail: state is unlocked; one writer (the USB task) is assumed. A single-bool race is harmless on Cortex-M.
 * Upgrade path: guard set/is_on with a lock if a second writer appears.
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
    gpio_put((uint)pf_led_board.gpio, (on == pf_led_board.active_high) ? 1u : 0u);
    return true;
}

bool pf_led_is_on(void)
{
    return led_available && led_on;
}
