// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

/**
 * @file led.h
 * @brief Board LED driver.
 */
#ifndef PF_LED_H
#define PF_LED_H

#include <stdbool.h>
#include <stdint.h>

/** LED GPIO value for boards without a direct GPIO LED. */
#define PF_LED_GPIO_NONE UINT8_MAX

/** Board-supplied LED assignment. */
typedef struct {
    uint8_t gpio; /**< LED GPIO, or PF_LED_GPIO_NONE (255). */
    bool active_high; /**< LED drive polarity. */
    bool (*put)(bool on); /**< Build-selected non-GPIO writer; NULL uses gpio and active_high. Returns true on success. */
} pf_led_board_t;

/** Build-selected assignment, defined in config/board.c. */
extern const pf_led_board_t pf_led_board;

/**
 * @brief Configure and clear the direct GPIO LED; non-GPIO writers initialize with their owning service.
 * @return true when configured or awaiting the non-GPIO driver; false for an invalid GPIO.
 */
bool pf_led_init(void);

/**
 * @brief Set the logical LED state.
 * @param on true to light the LED, regardless of the board polarity.
 * @return true when applied; false when the non-GPIO writer rejects it or the GPIO is invalid.
 */
bool pf_led_set(bool on);

/** Return the last successfully applied logical LED state. */
bool pf_led_is_on(void);

#endif
