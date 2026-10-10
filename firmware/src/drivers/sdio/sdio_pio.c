#include "sdio_pio.h"

#include <stddef.h>
#include <stdint.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "sdio_host.h"
#include "sdio_protocol.h"

static picoforge_sdio_host_t sdio_host;
static picoforge_sdio_config_t active_config;
static uint32_t card_sector_count;
static bool card_block_addressed;
static bool configuration_set;
static bool card_initialized;

static bool same_config(const picoforge_sdio_config_t *left,
                        const picoforge_sdio_config_t *right)
{
    return (left->command_gpio == right->command_gpio) &&
           (left->data0_gpio == right->data0_gpio) &&
           (left->pio == right->pio) &&
           (left->clock_hz == right->clock_hz) &&
           (left->dma_irq == right->dma_irq) &&
           (left->use_card_detect == right->use_card_detect) &&
           (left->card_detect_gpio == right->card_detect_gpio) &&
           (left->card_detect_active_high == right->card_detect_active_high) &&
           (left->card_detect_use_pull == right->card_detect_use_pull) &&
           (left->card_detect_pull_up == right->card_detect_pull_up);
}

static bool pins_are_valid(const picoforge_sdio_config_t *config)
{
    uint32_t pin_limit = (NUM_BANK0_GPIOS < 32u) ? NUM_BANK0_GPIOS : 32u;
    uint32_t data3_gpio;
    uint32_t clock_gpio;

    if (((config->pio != pio0) && (config->pio != pio1)) ||
        (config->data0_gpio < 2u) || (config->data0_gpio > 28u) ||
        (config->command_gpio >= pin_limit)) {
        return false;
    }

    data3_gpio = config->data0_gpio + 3u;
    clock_gpio = (config->data0_gpio + 30u) % 32u;
    if ((data3_gpio >= pin_limit) || (clock_gpio >= pin_limit) ||
        (config->command_gpio == clock_gpio) ||
        ((config->command_gpio >= config->data0_gpio) &&
         (config->command_gpio <= data3_gpio))) {
        return false;
    }

    if (config->use_card_detect &&
        ((config->card_detect_gpio >= pin_limit) ||
         (config->card_detect_gpio == config->command_gpio) ||
         (config->card_detect_gpio == clock_gpio) ||
         ((config->card_detect_gpio >= config->data0_gpio) &&
          (config->card_detect_gpio <= data3_gpio)))) {
        return false;
    }
    return true;
}

static bool config_is_valid(const picoforge_sdio_config_t *config)
{
    uint32_t clock_hz;

    if ((config == NULL) || !pins_are_valid(config) || (config->dma_irq > 1u)) {
        return false;
    }

    clock_hz = config->clock_hz;
    if (clock_hz == 0u) {
        clock_hz = clock_get_hz(clk_sys) / 12u;
    }
    return (clock_hz >= 400000u) && (clock_hz <= 25000000u) &&
           (clock_hz <= (clock_get_hz(clk_sys) / 2u));
}

static bool command_response(uint8_t command, uint32_t argument, uint8_t response[6],
                             uint32_t *response_argument)
{
    if (!picoforge_sdio_host_command_bytes(&sdio_host, command, argument, 48u,
                                          response, 6u) ||
        !picoforge_sdio_parse_response(command, response, response_argument, true)) {
        return false;
    }
    if (command == 8u) {
        return true;
    }
    if (command == 3u) {
        return (*response_argument & 0xe000u) == 0u;
    }
    return (*response_argument & 0xfdffe008u) == 0u;
}

static bool send_application_command(uint8_t command, uint32_t argument,
                                     uint32_t card_address,
                                     uint8_t response[6], uint32_t *response_argument)
{
    uint8_t prefix_response[6];
    uint32_t prefix_argument;

    return command_response(55u, card_address, prefix_response, &prefix_argument) &&
           command_response(command, argument, response, response_argument);
}

static bool wait_command_line_ready(void)
{
    uint32_t start = time_us_32();

    while (gpio_get(active_config.command_gpio) == 0u) {
        if ((uint32_t)(time_us_32() - start) > 1000000u) {
            return false;
        }
        tight_loop_contents();
    }
    return true;
}

static bool initialize_card(void)
{
    uint8_t response[17];
    uint8_t short_response[6];
    uint8_t csd[16];
    uint32_t response_argument;
    uint32_t ocr;
    uint32_t start;
    uint32_t acmd41_argument = 0x00ff8000u;
    bool supports_sd_v2;

    sleep_ms(2u);
    if (!picoforge_sdio_host_command_bytes(&sdio_host, 0u, 0u, 0u, NULL, 0u)) {
        return false;
    }

    if (!command_response(8u, 0x1aau, short_response, &response_argument) ||
        !picoforge_sdio_cmd8_supports_v2(response_argument, &supports_sd_v2)) {
        return false;
    }
    if (supports_sd_v2) {
        acmd41_argument |= 0x40000000u;
    }

    start = time_us_32();
    do {
        if (!command_response(55u, 0u, short_response, &response_argument) ||
            !picoforge_sdio_host_command_bytes(&sdio_host, 41u, acmd41_argument,
                                               48u, short_response, sizeof(short_response)) ||
            !picoforge_sdio_parse_r3_response(short_response, &ocr)) {
            return false;
        }
        if ((ocr & 0x80000000u) != 0u) {
            break;
        }
        sleep_ms(10u);
    } while ((uint32_t)(time_us_32() - start) < 1000000u);
    if ((ocr & 0x80000000u) == 0u) {
        return false;
    }
    card_block_addressed = supports_sd_v2 && ((ocr & 0x40000000u) != 0u);

    if (!picoforge_sdio_host_command_bytes(&sdio_host, 2u, 0u, 136u,
                                          response, sizeof(response)) ||
        !picoforge_sdio_validate_r2_response(response)) {
        return false;
    }
    if (!command_response(3u, 0u, short_response, &response_argument)) {
        return false;
    }
    response_argument = ((uint32_t)short_response[1] << 24u) |
                        ((uint32_t)short_response[2] << 16u) |
                        ((uint32_t)short_response[3] << 8u) |
                        (uint32_t)short_response[4];
    uint32_t relative_card_address = response_argument & 0xffff0000u;
    if (relative_card_address == 0u) {
        return false;
    }

    if (!picoforge_sdio_host_command_bytes(&sdio_host, 9u, relative_card_address,
                                          136u, response, sizeof(response)) ||
        !picoforge_sdio_validate_r2_response(response)) {
        return false;
    }
    for (size_t byte_index = 0u; byte_index < sizeof(csd); byte_index++) {
        csd[byte_index] = response[byte_index + 1u];
    }
    if (!picoforge_sdio_csd_sector_count(csd, &card_sector_count)) {
        return false;
    }

    if (!command_response(7u, relative_card_address, short_response, &response_argument) ||
        !wait_command_line_ready() ||
        !send_application_command(6u, 2u, relative_card_address,
                      short_response, &response_argument)) {
        return false;
    }
    if (!card_block_addressed &&
        !command_response(16u, PICOFORGE_SDIO_BLOCK_SIZE, short_response, &response_argument)) {
        return false;
    }

    uint32_t transfer_clock = active_config.clock_hz;
    if (transfer_clock == 0u) {
        transfer_clock = clock_get_hz(clk_sys) / 12u;
    }
    return picoforge_sdio_host_set_clock(&sdio_host, transfer_clock);
}

picoforge_sdio_result_t picoforge_sdio_init(const picoforge_sdio_config_t *config)
{
    if (!config_is_valid(config)) {
        return PICOFORGE_SDIO_INVALID_ARGUMENT;
    }

    if (configuration_set) {
        if (!same_config(&active_config, config)) {
            return PICOFORGE_SDIO_CONFIG_LOCKED;
        }
    } else {
        active_config = *config;
        configuration_set = true;
    }

    if (!sdio_host.pio) {
        if (active_config.use_card_detect) {
            gpio_init(active_config.card_detect_gpio);
            if (active_config.card_detect_use_pull) {
                if (active_config.card_detect_pull_up) {
                    gpio_pull_up(active_config.card_detect_gpio);
                } else {
                    gpio_pull_down(active_config.card_detect_gpio);
                }
            }
        }
        if (!picoforge_sdio_host_init(&sdio_host, &active_config)) {
            return PICOFORGE_SDIO_CARD_INIT_FAILED;
        }
    }

    if (active_config.use_card_detect &&
        ((gpio_get(active_config.card_detect_gpio) != 0u) != active_config.card_detect_active_high)) {
        card_initialized = false;
        return PICOFORGE_SDIO_CARD_INIT_FAILED;
    }
    if (!initialize_card()) {
        card_initialized = false;
        return PICOFORGE_SDIO_CARD_INIT_FAILED;
    }

    card_initialized = true;
    return PICOFORGE_SDIO_OK;
}

bool picoforge_sdio_is_initialized(void)
{
    return card_initialized;
}

uint32_t picoforge_sdio_block_count(void)
{
    return card_initialized ? card_sector_count : 0u;
}

static picoforge_sdio_result_t transfer_blocks(uint32_t first_block, uint8_t *read_buffer,
                                                const uint8_t *write_buffer, uint32_t block_count)
{
    uint32_t block_limit;

    if (!card_initialized) {
        return PICOFORGE_SDIO_NOT_READY;
    }
    if (block_count == 0u) {
        return PICOFORGE_SDIO_OK;
    }
    if (((read_buffer == NULL) && (write_buffer == NULL)) ||
        (((uintptr_t)((read_buffer != NULL) ? (const void *)read_buffer : (const void *)write_buffer) & 3u) != 0u)) {
        return PICOFORGE_SDIO_INVALID_ARGUMENT;
    }

    block_limit = picoforge_sdio_block_count();
    if ((first_block >= block_limit) || (block_count > (block_limit - first_block))) {
        return PICOFORGE_SDIO_INVALID_ARGUMENT;
    }

    while (block_count > 0u) {
        if (active_config.use_card_detect &&
            ((gpio_get(active_config.card_detect_gpio) != 0u) != active_config.card_detect_active_high)) {
            card_initialized = false;
            return PICOFORGE_SDIO_NOT_READY;
        }
        bool transferred = (read_buffer != NULL)
            ? picoforge_sdio_host_read_sector(&sdio_host, first_block, card_block_addressed, read_buffer)
            : picoforge_sdio_host_write_sector(&sdio_host, first_block, card_block_addressed, write_buffer);
        if (!transferred) {
            return PICOFORGE_SDIO_IO_ERROR;
        }
        if (read_buffer != NULL) {
            read_buffer += PICOFORGE_SDIO_BLOCK_SIZE;
        } else {
            write_buffer += PICOFORGE_SDIO_BLOCK_SIZE;
        }
        first_block++;
        block_count--;
    }
    return PICOFORGE_SDIO_OK;
}

picoforge_sdio_result_t picoforge_sdio_read_blocks(uint32_t first_block, void *buffer, uint32_t block_count)
{
    return transfer_blocks(first_block, buffer, NULL, block_count);
}

picoforge_sdio_result_t picoforge_sdio_write_blocks(uint32_t first_block, const void *buffer,
                                                    uint32_t block_count)
{
    return transfer_blocks(first_block, NULL, buffer, block_count);
}

picoforge_sdio_result_t picoforge_sdio_sync(void)
{
    return card_initialized ? PICOFORGE_SDIO_OK : PICOFORGE_SDIO_NOT_READY;
}