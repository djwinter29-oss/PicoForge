#ifndef PF_LED_H
#define PF_LED_H

#include <stdbool.h>
#include <stdint.h>

/** Configure the board LED GPIO and clear it; returns false for invalid pins. */
bool pf_led_init(uint32_t gpio, bool active_high);

/** Return whether a board LED has been configured. */
bool pf_led_is_available(void);

/** Set the logical LED state; returns false when the board has no LED. */
bool pf_led_set(bool on);

/** Return the logical LED state, or false when unavailable/off. */
bool pf_led_is_on(void);

#endif