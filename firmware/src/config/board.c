/** @file
 * @brief Build-selected board assignments; each object is declared extern in its driver header.
 */
#include "platform/led.h"
#include "platform/logging.h"
#include "config/config.h"

#if PF_ENABLE_WIRELESS
#include "app/wireless/wireless.h"
#endif

const pf_led_board_t pf_led_board = {
    .gpio = PF_BOARD_LED_GPIO,
    .active_high = !PICO_DEFAULT_LED_PIN_INVERTED,
#if PF_ENABLE_WIRELESS
    .put = pf_wireless_led_put,
#endif
};

const pf_logging_board_t pf_logging_board = {
    .uart_index = PICO_DEFAULT_UART,
    .tx_gpio = PICO_DEFAULT_UART_TX_PIN,
    .rx_gpio = PICO_DEFAULT_UART_RX_PIN,
    .baud_rate = 115200u,
    .default_level = PF_LOGGING_LEVEL_INFO,
};
