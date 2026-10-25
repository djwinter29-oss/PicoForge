#ifndef PF_CONFIG_CONFIG_H
#define PF_CONFIG_CONFIG_H

#include <stdint.h>

#include "pico/stdlib.h"

#ifndef PICO_DEFAULT_UART
/** Fallback UART controller when the SDK board has no default. */
#define PICO_DEFAULT_UART 0u
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
/** Fallback monitor UART TX GPIO. */
#define PICO_DEFAULT_UART_TX_PIN 0u
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
/** Fallback monitor UART RX GPIO. */
#define PICO_DEFAULT_UART_RX_PIN 1u
#endif
#ifndef PICO_DEFAULT_LED_PIN_INVERTED
/** Default to an active-high board LED. */
#define PICO_DEFAULT_LED_PIN_INVERTED 0
#endif

#ifdef PICO_DEFAULT_LED_PIN
/** SDK board LED GPIO, or 255 when no LED is assigned. */
#define PF_BOARD_LED_GPIO PICO_DEFAULT_LED_PIN
#else
/** No LED GPIO assigned by the SDK board. */
#define PF_BOARD_LED_GPIO UINT8_MAX
#endif

#ifndef PF_CONFIG_SDCARD_INTERRUPT_WAIT
#define PF_CONFIG_SDCARD_INTERRUPT_WAIT 0
#endif

#endif