#ifndef PF_LED_H
#define PF_LED_H

#include <stdbool.h>
#include <stdint.h>

/** LED GPIO value for boards without a direct GPIO LED. */
#define PF_LED_GPIO_NONE UINT32_MAX

/** Board-supplied LED assignment. */
typedef struct {
    uint32_t gpio; /**< LED GPIO, or PF_LED_GPIO_NONE. */
    bool active_high; /**< LED drive polarity. */
} pf_led_board_t;

/** Build-selected assignment, defined in config/board.c. */
extern const pf_led_board_t pf_led_board;

/**
 * @brief Configure the pf_led_board GPIO and clear it; later calls are no-ops.
 * @return true when the board has no LED or it was configured; false for an invalid GPIO.
 */
bool pf_led_init(void);

/** Return whether a board LED has been configured. */
bool pf_led_is_available(void);

/** Set the logical LED state; returns false when the board has no LED. */
bool pf_led_set(bool on);

/** Return the logical LED state, or false when unavailable/off. */
bool pf_led_is_on(void);

#endif