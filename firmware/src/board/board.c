#include "board.h"

#include "config/config.h"
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SDIO_PIO
#include "drivers/storage/sdio_pio/storage_sdio_pio.h"
static pf_storage_sdio_pio_t sd_transport;
#else
#include "drivers/storage/spi/storage_spi.h"
static pf_storage_spi_t sd_transport;
#endif
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico/stdio_uart.h"
#include "led.h"

static bool monitor_uart_initialized;
static bool led_initialized;
static bool storage_configured;

/** @brief Sample board-owned card detect; the immutable profile has static lifetime. */
static bool card_present(void *context)
{
    const pf_board_resources_t *profile = context;
    return (gpio_get(profile->card_detect_gpio) != 0u) == profile->card_detect_active_high;
}

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

bool pf_board_get_sd_card_config(pf_sd_config_t *config)
{
    const pf_board_resources_t *topology = pf_board_resources_get();

    if ((config == NULL) || !topology->sd_card_enabled ||
        !pf_board_resources_validate(topology)) {
        return false;
    }

#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SDIO_PIO
    if (!storage_configured) {
        sd_transport.config = (pf_storage_sdio_pio_config_t){
            .pins = topology->native_pins,
            .pio = topology->pio_index == 0u ? pio0 : pio1,
        };
    }
    config->transport = PF_SD_TRANSPORT_SDIO_PIO;
    config->bus.native = (pf_sd_native_config_t){
        .transport = pf_storage_sdio_pio_transport(&sd_transport),
        .clock_hz = topology->clock_hz,
    };
    pf_storage_transport_t *transport = &config->bus.native.transport;
#else
    if (!storage_configured) {
        sd_transport = (pf_storage_spi_t){
            .instance = topology->spi_index == 0u ? spi0 : spi1,
            .pins = topology->spi_pins, .clock_hz = 400000u,
        };
    }
    config->transport = PF_SD_TRANSPORT_SPI;
    config->bus.spi = (pf_sd_spi_config_t){
        .transport = pf_storage_spi_transport(&sd_transport),
        .clock_hz = topology->spi_clock_hz,
    };
    pf_storage_transport_t *transport = &config->bus.spi.transport;
#endif
    if (topology->card_detect_enabled) {
        if (!storage_configured) {
            gpio_init(topology->card_detect_gpio);
            if (topology->card_detect_use_pull) {
                gpio_set_pulls(topology->card_detect_gpio, topology->card_detect_pull_up,
                               !topology->card_detect_pull_up);
            } else {
                gpio_disable_pulls(topology->card_detect_gpio);
            }
        }
        transport->present = card_present;
        transport->presence_context = (void *)topology;
    }
    storage_configured = true;
    return true;
}