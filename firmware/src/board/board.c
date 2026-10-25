#include "board.h"

#include "config/config.h"
#include "drivers/boot_config.h"
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SDIO_PIO
#include "drivers/storage/sdio_pio/sdio_pio.h"
static pf_sdio_pio_t sd_transport;
#else
#include "drivers/storage/spi/spi.h"
static pf_spi_t sd_transport;
#endif
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico/stdio_uart.h"
#include "led.h"

static bool monitor_uart_initialized;
static bool led_initialized;
static bool storage_configured;

/** @brief Sample board-owned card detect; the immutable profile has static lifetime. */
static bool card_present(const void *context)
{
    const pf_board_resources_t *profile = context;
    return (gpio_get(profile->card_detect_gpio) != 0u) == profile->card_detect_active_high;
}

bool pf_board_init(void)
{
    const pf_board_resources_t *topology = pf_board_resources_get();

    if (!monitor_uart_initialized) {
        uart_inst_t *uart = topology->monitor_uart_index == 0u ? uart0 : uart1;
        stdio_uart_init_full(uart, topology->monitor_uart_baud_rate,
                             (int)topology->monitor_uart_tx_gpio,
                             (int)topology->monitor_uart_rx_gpio);
        stdio_filter_driver(&stdio_uart);
        monitor_uart_initialized = true;
    }

    if (topology->led_gpio != PF_BOARD_PIN_UNASSIGNED && !led_initialized) {
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
    const pf_boot_config_t *boot_config = pf_boot_config_active();

    if (config == NULL) {
        return false;
    }

#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SDIO_PIO
    if (!storage_configured) {
        sd_transport.config = (pf_sdio_pio_config_t){
            .pins = topology->sdio_pio_pins,
            .pio = topology->pio_index == 0u ? pio0 : pio1,
        };
    }
    *config = (pf_sd_config_t){
        .transport = pf_sdio_pio_bus(&sd_transport),
        .clock_hz = topology->sd_clock_hz,
        .native_default_speed_only = boot_config->sdio_bus_mhz != 50u,
    };
#else
    if (!storage_configured) {
        sd_transport = (pf_spi_t){
            .instance = topology->spi_index == 0u ? spi0 : spi1,
            .pins = topology->spi_pins,
            .clock_hz = 400000u,
        };
    }
    *config = (pf_sd_config_t){
        .transport = pf_spi_bus(&sd_transport),
        .clock_hz = boot_config->spi_bus_mhz != 0u ? boot_config->spi_bus_mhz * 1000000u
                                                   : topology->sd_clock_hz,
    };
#endif
    if (!storage_configured) {
        gpio_init(topology->card_detect_gpio);
        gpio_disable_pulls(topology->card_detect_gpio);
    }
    config->transport.present = card_present;
    config->transport.presence_context = topology;
    storage_configured = true;
    return true;
}