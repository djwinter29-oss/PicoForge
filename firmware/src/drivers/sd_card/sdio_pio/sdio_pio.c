#include "sdio_pio.h"

#include <stddef.h>
#include <stdint.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "sdio_pio_host.h"
#include "sdio_pio_protocol.h"
#include "sd_card_protocol.h"

static pf_sdio_pio_host_t sdio_pio_host;
static pf_sdio_pio_config_t active_config;
static uint32_t card_sector_count;
static bool card_block_addressed;
static bool configuration_set;
static bool card_initialized;

static bool same_config(const pf_sdio_pio_config_t *left,
                        const pf_sdio_pio_config_t *right)
{
    if ((left->command_gpio != right->command_gpio) ||
        (left->clock_gpio != right->clock_gpio) ||
        (left->pio != right->pio) ||
        (left->clock_hz != right->clock_hz) ||
        (left->dma_irq != right->dma_irq) ||
        (left->use_card_detect != right->use_card_detect) ||
        (left->card_detect_gpio != right->card_detect_gpio) ||
        (left->card_detect_active_high != right->card_detect_active_high) ||
        (left->card_detect_use_pull != right->card_detect_use_pull) ||
        (left->card_detect_pull_up != right->card_detect_pull_up)) {
        return false;
    }
    for (uint32_t lane = 0u; lane < 4u; lane++) {
        if (left->data_gpio[lane] != right->data_gpio[lane]) {
            return false;
        }
    }
    return true;
}

static bool data_gpio_window(const uint32_t data_gpio[4], uint32_t pin_limit,
                             uint32_t *gpio_base)
{
    bool used_offsets[4] = { false, false, false, false };
    uint32_t base = UINT32_MAX;

    for (uint32_t lane = 0u; lane < 4u; lane++) {
        if (data_gpio[lane] >= pin_limit) {
            return false;
        }
        if (data_gpio[lane] < base) {
            base = data_gpio[lane];
        }
    }
    for (uint32_t lane = 0u; lane < 4u; lane++) {
        uint32_t offset = data_gpio[lane] - base;
        if ((offset >= 4u) || used_offsets[offset]) {
            return false;
        }
        used_offsets[offset] = true;
    }
    *gpio_base = base;
    return true;
}

static bool pin_is_data_gpio(uint32_t pin, const uint32_t data_gpio[4])
{
    for (uint32_t lane = 0u; lane < 4u; lane++) {
        if (pin == data_gpio[lane]) {
            return true;
        }
    }
    return false;
}

static bool pins_are_valid(const pf_sdio_pio_config_t *config)
{
    uint32_t pin_limit = (NUM_BANK0_GPIOS < 32u) ? NUM_BANK0_GPIOS : 32u;
    uint32_t data_gpio_base;

    if (((config->pio != pio0) && (config->pio != pio1)) ||
        !data_gpio_window(config->data_gpio, pin_limit, &data_gpio_base) ||
        (data_gpio_base < 2u) || (data_gpio_base > 28u) ||
        (config->clock_gpio >= pin_limit) ||
        (config->clock_gpio != ((data_gpio_base + 30u) % 32u)) ||
        (config->command_gpio >= pin_limit)) {
        return false;
    }

    if ((config->command_gpio == config->clock_gpio) ||
        pin_is_data_gpio(config->command_gpio, config->data_gpio)) {
        return false;
    }

    if (config->use_card_detect &&
        ((config->card_detect_gpio >= pin_limit) ||
         (config->card_detect_gpio == config->command_gpio) ||
         (config->card_detect_gpio == config->clock_gpio) ||
         pin_is_data_gpio(config->card_detect_gpio, config->data_gpio))) {
        return false;
    }
    return true;
}

static bool config_is_valid(const pf_sdio_pio_config_t *config)
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
    if (!pf_sdio_pio_host_command_bytes(&sdio_pio_host, command, argument, 48u,
                                          response, 6u) ||
        !pf_sdio_pio_parse_response(command, response, response_argument, true)) {
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
    if (!pf_sdio_pio_host_command_bytes(&sdio_pio_host, 0u, 0u, 0u, NULL, 0u)) {
        return false;
    }

    if (!command_response(8u, 0x1aau, short_response, &response_argument) ||
        !pf_sdio_pio_cmd8_supports_v2(response_argument, &supports_sd_v2)) {
        return false;
    }
    if (supports_sd_v2) {
        acmd41_argument |= 0x40000000u;
    }

    start = time_us_32();
    do {
        if (!command_response(55u, 0u, short_response, &response_argument) ||
            !pf_sdio_pio_host_command_bytes(&sdio_pio_host, 41u, acmd41_argument,
                                               48u, short_response, sizeof(short_response)) ||
            !pf_sdio_pio_parse_r3_response(short_response, &ocr)) {
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

    if (!pf_sdio_pio_host_command_bytes(&sdio_pio_host, 2u, 0u, 136u,
                                          response, sizeof(response)) ||
        !pf_sdio_pio_validate_r2_response(response)) {
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

    if (!pf_sdio_pio_host_command_bytes(&sdio_pio_host, 9u, relative_card_address,
                                          136u, response, sizeof(response)) ||
        !pf_sdio_pio_validate_r2_response(response)) {
        return false;
    }
    for (size_t byte_index = 0u; byte_index < sizeof(csd); byte_index++) {
        csd[byte_index] = response[byte_index + 1u];
    }
    if (!pf_sdio_pio_csd_sector_count(csd, &card_sector_count)) {
        return false;
    }

    if (!command_response(7u, relative_card_address, short_response, &response_argument) ||
        !wait_command_line_ready() ||
        !send_application_command(6u, 2u, relative_card_address,
                      short_response, &response_argument)) {
        return false;
    }
    if (!card_block_addressed &&
        !command_response(16u, PF_SD_CARD_BLOCK_SIZE, short_response, &response_argument)) {
        return false;
    }

    uint32_t transfer_clock = active_config.clock_hz;
    if (transfer_clock == 0u) {
        transfer_clock = clock_get_hz(clk_sys) / 12u;
    }
    return pf_sdio_pio_host_set_clock(&sdio_pio_host, transfer_clock);
}

pf_sd_card_result_t pf_sdio_pio_init(const pf_sdio_pio_config_t *config)
{
    if (!config_is_valid(config)) {
        return PF_SD_CARD_INVALID_ARGUMENT;
    }

    if (configuration_set) {
        if (!same_config(&active_config, config)) {
            return PF_SD_CARD_CONFIG_LOCKED;
        }
    } else {
        active_config = *config;
        configuration_set = true;
    }

    if (!sdio_pio_host.pio) {
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
        if (!pf_sdio_pio_host_init(&sdio_pio_host, &active_config)) {
            return PF_SD_CARD_INIT_FAILED;
        }
    }

    if (active_config.use_card_detect &&
        ((gpio_get(active_config.card_detect_gpio) != 0u) != active_config.card_detect_active_high)) {
        card_initialized = false;
        return PF_SD_CARD_INIT_FAILED;
    }
    if (!initialize_card()) {
        card_initialized = false;
        return PF_SD_CARD_INIT_FAILED;
    }

    card_initialized = true;
    return PF_SD_CARD_OK;
}

bool pf_sdio_pio_is_initialized(void)
{
    return card_initialized;
}

uint32_t pf_sdio_pio_block_count(void)
{
    return card_initialized ? card_sector_count : 0u;
}

static pf_sd_card_result_t transfer_blocks(uint32_t first_block, uint8_t *read_buffer,
                                                const uint8_t *write_buffer, uint32_t block_count)
{
    pf_sd_card_result_t result;

    if (!card_initialized) {
        return PF_SD_CARD_NOT_READY;
    }
    if (block_count == 0u) {
        return PF_SD_CARD_OK;
    }
    if (((read_buffer == NULL) && (write_buffer == NULL)) ||
        (((uintptr_t)((read_buffer != NULL) ? (const void *)read_buffer : (const void *)write_buffer) & 3u) != 0u)) {
        return PF_SD_CARD_INVALID_ARGUMENT;
    }

    result = pf_sd_card_validate_transfer(first_block, read_buffer != NULL ? read_buffer : write_buffer,
                                          block_count, pf_sdio_pio_block_count());
    if (result != PF_SD_CARD_OK) {
        return result;
    }

    while (block_count > 0u) {
        if (active_config.use_card_detect &&
            ((gpio_get(active_config.card_detect_gpio) != 0u) != active_config.card_detect_active_high)) {
            card_initialized = false;
            return PF_SD_CARD_NOT_READY;
        }
        bool transferred = (read_buffer != NULL)
            ? pf_sdio_pio_host_read_sector(&sdio_pio_host, first_block, card_block_addressed, read_buffer)
            : pf_sdio_pio_host_write_sector(&sdio_pio_host, first_block, card_block_addressed, write_buffer);
        if (!transferred) {
            return PF_SD_CARD_IO_ERROR;
        }
        if (read_buffer != NULL) {
            read_buffer += PF_SD_CARD_BLOCK_SIZE;
        } else {
            write_buffer += PF_SD_CARD_BLOCK_SIZE;
        }
        first_block++;
        block_count--;
    }
    return PF_SD_CARD_OK;
}

pf_sd_card_result_t pf_sdio_pio_read_blocks(uint32_t first_block, void *buffer, uint32_t block_count)
{
    return transfer_blocks(first_block, buffer, NULL, block_count);
}

pf_sd_card_result_t pf_sdio_pio_write_blocks(uint32_t first_block, const void *buffer,
                                                    uint32_t block_count)
{
    return transfer_blocks(first_block, NULL, buffer, block_count);
}

pf_sd_card_result_t pf_sdio_pio_sync(void)
{
    return card_initialized ? PF_SD_CARD_OK : PF_SD_CARD_NOT_READY;
}