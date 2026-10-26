#include <assert.h>
#include <stdint.h>

#include "platform/led.h"
#include "hardware/gpio.h"

#ifndef PF_TEST_LED_GPIO
#define PF_TEST_LED_GPIO 25u
#endif
#ifndef PF_TEST_LED_ACTIVE_HIGH
#define PF_TEST_LED_ACTIVE_HIGH true
#endif

const pf_led_board_t pf_led_board = {.gpio = PF_TEST_LED_GPIO, .active_high = PF_TEST_LED_ACTIVE_HIGH};

static unsigned init_calls;
static int last_level = -1;

void gpio_init(uint gpio) { assert(gpio == PF_TEST_LED_GPIO); ++init_calls; }
void gpio_set_dir(uint gpio, bool out) { assert(gpio == PF_TEST_LED_GPIO); assert(out); }
void gpio_put(uint gpio, bool value) { assert(gpio == PF_TEST_LED_GPIO); last_level = value; }

int main(void)
{
    assert(!pf_led_is_available() && !pf_led_is_on() && !pf_led_set(true));
#if PF_TEST_LED_GPIO == PF_LED_GPIO_NONE
    assert(pf_led_init() && !pf_led_is_available() && !pf_led_set(true) && init_calls == 0u);
    return 0;
#elif PF_TEST_LED_GPIO >= 30u
    assert(!pf_led_init() && !pf_led_is_available() && init_calls == 0u);
    return 0;
#else
    const int off_level = PF_TEST_LED_ACTIVE_HIGH ? 0 : 1;
    const int on_level = 1 - off_level;

    assert(pf_led_init() && pf_led_is_available() && !pf_led_is_on());
    assert(init_calls == 1u && last_level == off_level);
    assert(pf_led_set(true) && pf_led_is_on() && last_level == on_level);
    assert(pf_led_set(false) && !pf_led_is_on() && last_level == off_level);
    assert(pf_led_init() && init_calls == 1u);
    return 0;
#endif
}
