#include "board.h"

#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

static const pf_board_topology_t board_topology = {
    .gpio_count = NUM_BANK0_GPIOS,
#ifdef PICO_DEFAULT_LED_PIN
    .led_enabled = true,
    .led_gpio = PICO_DEFAULT_LED_PIN,
    .led_active_high = true,
#else
    .led_enabled = false,
    .led_gpio = PF_BOARD_PIN_UNASSIGNED,
#endif
    .sdio_enabled = false,
    .command_gpio = PF_BOARD_PIN_UNASSIGNED,
    .data0_gpio = PF_BOARD_PIN_UNASSIGNED,
    .pio_index = 0u,
    .clock_hz = 0u,
    .card_detect_enabled = false,
    .card_detect_gpio = PF_BOARD_PIN_UNASSIGNED,
};

const pf_board_topology_t *pf_board_get_topology(void)
{
    return &board_topology;
}

bool pf_board_get_led_config(pf_board_led_config_t *config)
{
    if ((config == NULL) || !board_topology.led_enabled ||
        !pf_board_topology_validate(&board_topology)) {
        return false;
    }

    *config = (pf_board_led_config_t){
        .gpio = board_topology.led_gpio,
        .active_high = board_topology.led_active_high,
    };
    return true;
}

bool pf_board_get_sdio_config(pf_sdio_config_t *config)
{
    if ((config == NULL) || !board_topology.sdio_enabled ||
        !pf_board_topology_validate(&board_topology)) {
        return false;
    }

    *config = (pf_sdio_config_t){
        .command_gpio = board_topology.command_gpio,
        .data0_gpio = board_topology.data0_gpio,
        .pio = (board_topology.pio_index == 0u) ? pio0 : pio1,
        .clock_hz = board_topology.clock_hz,
        .dma_irq = 0u,
        .use_card_detect = board_topology.card_detect_enabled,
        .card_detect_gpio = board_topology.card_detect_gpio,
        .card_detect_active_high = board_topology.card_detect_active_high,
        .card_detect_use_pull = board_topology.card_detect_use_pull,
        .card_detect_pull_up = board_topology.card_detect_pull_up,
    };
    return true;
}