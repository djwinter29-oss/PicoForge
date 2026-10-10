#ifndef PICOFORGE_LED_H
#define PICOFORGE_LED_H

#include <stdbool.h>
#include <stdint.h>

/** Configure the board LED GPIO and clear it; returns false for invalid pins. */
bool picoforge_led_init(uint32_t gpio, bool active_high);

/** Return whether a board LED has been configured. */
bool picoforge_led_is_available(void);

/** Set the logical LED state; returns false when the board has no LED. */
bool picoforge_led_set(bool on);

/** Return the logical LED state, or false when unavailable/off. */
bool picoforge_led_is_on(void);

#endif