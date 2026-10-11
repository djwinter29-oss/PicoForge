#include "board.h"

#include "config/config.h"
#include "hardware/pio.h"
#include "hardware/spi.h"
#include "hardware/uart.h"
#include "pico/stdio_uart.h"
#include "led.h"

#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SDIO_PIO
#define PF_CONFIGURED_SD_CARD_TRANSPORT PF_SD_CARD_TRANSPORT_SDIO_PIO
#else
#define PF_CONFIGURED_SD_CARD_TRANSPORT PF_SD_CARD_TRANSPORT_SPI
#endif

static bool monitor_uart_initialized;
static bool led_initialized;

bool pf_board_init(void)
{
    const pf_board_resources_t *topology = pf_board_resources_get();
    uart_inst_t *uart;

    if (!pf_board_resources_validate(topology) || !topology->monitor_uart_enabled) {
        return false;
    }

    if (!monitor_uart_initialized) {
        uart = (topology->monitor_uart_index == 0u) ? uart0 : uart1;
        stdio_uart_init_full(uart, topology->monitor_uart_baud_rate,
                             (int)topology->monitor_uart_tx_gpio,
                             (int)topology->monitor_uart_rx_gpio);
        monitor_uart_initialized = true;
    }

    if (topology->led_enabled && !led_initialized) {
        if (!pf_led_init(topology->led_gpio, topology->led_active_high)) {
            return false;
        }
        led_initialized = true;
    }
    return true;
}

bool pf_board_get_sd_card_config(pf_sd_card_config_t *config)
{
    const pf_board_resources_t *topology = pf_board_resources_get();
    const pf_sd_card_transport_t transport = PF_CONFIGURED_SD_CARD_TRANSPORT;

    if ((config == NULL) || !topology->sd_card_enabled ||
        !pf_board_resources_validate(topology)) {
        return false;
    }

    config->transport = transport;
    if (transport == PF_SD_CARD_TRANSPORT_SDIO_PIO) {
        config->bus.sdio_pio = (pf_sdio_pio_config_t){
            .command_gpio = topology->command_gpio,
            .clock_gpio = topology->clock_gpio,
            .data_gpio = { topology->data_gpio[0], topology->data_gpio[1],
                           topology->data_gpio[2], topology->data_gpio[3] },
            .pio = (topology->pio_index == 0u) ? pio0 : pio1,
            .clock_hz = topology->clock_hz,
            .use_card_detect = topology->card_detect_enabled,
            .card_detect_gpio = topology->card_detect_gpio,
            .card_detect_active_high = topology->card_detect_active_high,
            .card_detect_use_pull = topology->card_detect_use_pull,
            .card_detect_pull_up = topology->card_detect_pull_up,
        };
        return true;
    }
    if (transport == PF_SD_CARD_TRANSPORT_SPI) {
        config->bus.spi = (pf_sd_spi_config_t){
            .spi = (topology->spi_index == 0u) ? spi0 : spi1,
            .clock_gpio = topology->spi_clock_gpio,
            .mosi_gpio = topology->spi_mosi_gpio,
            .miso_gpio = topology->spi_miso_gpio,
            .chip_select_gpio = topology->spi_chip_select_gpio,
            .clock_hz = topology->spi_clock_hz,
            .use_card_detect = topology->card_detect_enabled,
            .card_detect_gpio = topology->card_detect_gpio,
            .card_detect_active_high = topology->card_detect_active_high,
            .card_detect_use_pull = topology->card_detect_use_pull,
            .card_detect_pull_up = topology->card_detect_pull_up,
        };
        return true;
    }
    return false;
}