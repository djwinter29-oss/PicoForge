#include "led.h"

#include "hardware/gpio.h"

static uint32_t led_gpio;
static bool led_active_high;
static bool led_available;
static bool led_on;

bool pf_led_init(void)
{
    if (led_available || pf_led_board.gpio == PF_LED_GPIO_NONE) {
        return true;
    }
    if (pf_led_board.gpio >= NUM_BANK0_GPIOS) {
        return false;
    }

    led_gpio = pf_led_board.gpio;
    led_active_high = pf_led_board.active_high;
    led_on = false;
    led_available = true;
    gpio_init((uint)led_gpio);
    gpio_set_dir((uint)led_gpio, GPIO_OUT);
    gpio_put((uint)led_gpio, led_active_high ? 0u : 1u);
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
    gpio_put((uint)led_gpio, (on == led_active_high) ? 1u : 0u);
    return true;
}

bool pf_led_is_on(void)
{
    return led_available && led_on;
}