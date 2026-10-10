#include "led.h"

#include "hardware/gpio.h"

static uint32_t led_gpio;
static bool led_active_high;
static bool led_available;
static bool led_on;

bool picoforge_led_init(uint32_t gpio, bool active_high)
{
    if (gpio >= NUM_BANK0_GPIOS) {
        led_available = false;
        led_on = false;
        return false;
    }

    led_gpio = gpio;
    led_active_high = active_high;
    led_on = false;
    led_available = true;
    gpio_init((uint)led_gpio);
    gpio_set_dir((uint)led_gpio, GPIO_OUT);
    gpio_put((uint)led_gpio, led_active_high ? 0u : 1u);
    return true;
}

bool picoforge_led_is_available(void)
{
    return led_available;
}

bool picoforge_led_set(bool on)
{
    if (!led_available) {
        return false;
    }

    led_on = on;
    gpio_put((uint)led_gpio, (on == led_active_high) ? 1u : 0u);
    return true;
}

bool picoforge_led_is_on(void)
{
    return led_available && led_on;
}