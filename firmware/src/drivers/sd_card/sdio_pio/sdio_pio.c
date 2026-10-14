#include "sdio_pio.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "sdio_pio_host.h"
#include "sdio_pio_protocol.h"
#include "sd_card_protocol.h"

static pf_sdio_pio_host_t sdio_pio_host;
static pf_sdio_pio_config_t active_config;
static uint32_t card_sector_count;
static uint8_t card_cid[16];
static bool card_block_addressed;
static bool configuration_set;
static bool card_initialized;
static pf_sdio_pio_init_stage_t card_init_stage;
static uint8_t card_init_response[6];
static uint8_t last_short_command;
static uint32_t last_short_argument;
static uint32_t last_short_response_argument;

/* Capped so a faster system clock does not raise the card bus above the verified rate. */
static uint32_t default_clock_hz(void)
{
    uint32_t clock_hz = clock_get_hz(clk_sys) / 12u;

    return (clock_hz > PF_SDIO_PIO_DEFAULT_CLOCK_MAX_HZ) ? PF_SDIO_PIO_DEFAULT_CLOCK_MAX_HZ
                                                         : clock_hz;
}

static bool same_config(const pf_sdio_pio_config_t *left,
                        const pf_sdio_pio_config_t *right)
{
    if ((left->command_gpio != right->command_gpio) ||
        (left->clock_gpio != right->clock_gpio) ||
        (left->pio != right->pio) ||
        (left->clock_hz != right->clock_hz) ||
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
    uint32_t base = data_gpio[0];

    for (uint32_t lane = 0u; lane < 4u; lane++) {
        if ((data_gpio[lane] >= pin_limit) ||
            (data_gpio[lane] != base + lane)) {
            return false;
        }
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

    if ((config == NULL) || !pins_are_valid(config)) {
        return false;
    }

    clock_hz = config->clock_hz;
    if (clock_hz == 0u) {
        clock_hz = default_clock_hz();
    }
    return (clock_hz >= 400000u) && (clock_hz <= 50000000u) &&
           (clock_hz <= (clock_get_hz(clk_sys) / 2u));
}

static bool command_response(uint8_t command, uint32_t argument, uint8_t response[6],
                             uint32_t *response_argument)
{
    memset(card_init_response, 0, sizeof(card_init_response));
    if (!pf_sdio_pio_host_command_bytes(&sdio_pio_host, command, argument, 48u,
                                       response, 6u)) {
        return false;
    }
    memcpy(card_init_response, response, sizeof(card_init_response));
    if (!pf_sdio_pio_parse_response(command, response, response_argument, true)) {
        return false;
    }
    last_short_command = command;
    last_short_argument = argument;
    last_short_response_argument = *response_argument;
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

static bool wait_card_data_ready(void)
{
    uint32_t start = time_us_32();

    while (gpio_get(active_config.data_gpio[0]) == 0u) {
        if ((uint32_t)(time_us_32() - start) > 1000000u) {
            return false;
        }
        pf_sd_card_idle();
    }
    return true;
}

static const uint32_t transfer_clock_steps_hz[] = {
    50000000u, 40000000u, 33000000u, 25000000u, 20000000u, 10000000u, 5000000u, 1000000u,
};
static bool probe_transfer(void)
{
    static uint8_t read_probe[64u * PF_SD_CARD_BLOCK_SIZE] __attribute__((aligned(4)));
    uint32_t probe_clock_hz = sdio_pio_host.current_clock_hz;
    uint32_t probe_sector_count = (card_sector_count < 256u) ? card_sector_count : 256u;

    for (uint32_t first_sector = 0u; first_sector < probe_sector_count;) {
        uint32_t batch_count = probe_sector_count - first_sector;
        if (batch_count > 64u) {
            batch_count = 64u;
        }
        if (!pf_sdio_pio_host_read_blocks(&sdio_pio_host, first_sector,
                                          card_block_addressed, read_probe, batch_count) ||
            (sdio_pio_host.current_clock_hz != probe_clock_hz) ||
            ((probe_clock_hz > 20000000u) &&
             !sdio_pio_host.data_rx_multiblock_supported)) {
            return false;
        }
        first_sector += batch_count;
    }
    return true;
}

static bool initialize_card(uint32_t *clock_limit_hz)
{
    uint8_t response[17];
    uint8_t short_response[6];
    uint8_t csd[16];
    uint8_t scr[8];
    uint32_t response_argument;
    uint32_t ocr;
    uint32_t start;
    uint32_t card_default_transfer_hz;
    uint32_t acmd41_argument = 0x00ff8000u;
    bool supports_sd_v2;

    card_init_stage = PF_SDIO_PIO_INIT_STAGE_IDLE;
    sleep_ms(2u);
    if (!pf_sdio_pio_host_command_bytes(&sdio_pio_host, 0u, 0u, 0u, NULL, 0u)) {
        return false;
    }

    card_init_stage = PF_SDIO_PIO_INIT_STAGE_CMD8_RESPONSE;
    if (!command_response(8u, 0x1aau, short_response, &response_argument)) {
        return false;
    }
    card_init_stage = PF_SDIO_PIO_INIT_STAGE_CMD8_CLASSIFY;
    if (!pf_sdio_pio_cmd8_supports_v2(response_argument, &supports_sd_v2)) {
        return false;
    }
    if (supports_sd_v2) {
        acmd41_argument |= 0x40000000u;
    }

    card_init_stage = PF_SDIO_PIO_INIT_STAGE_OPERATING_CONDITION;
    start = time_us_32();
    do {
        card_init_stage = PF_SDIO_PIO_INIT_STAGE_OPERATING_PREFIX;
        if (!command_response(55u, 0u, short_response, &response_argument)) {
            return false;
        }
        card_init_stage = PF_SDIO_PIO_INIT_STAGE_OPERATING_RESPONSE;
        memset(card_init_response, 0, sizeof(card_init_response));
        if (!pf_sdio_pio_host_command_bytes(&sdio_pio_host, 41u, acmd41_argument,
                                            48u, short_response, sizeof(short_response))) {
            return false;
        }
        memcpy(card_init_response, short_response, sizeof(card_init_response));
        card_init_stage = PF_SDIO_PIO_INIT_STAGE_OPERATING_PARSE;
        if (!pf_sdio_pio_parse_r3_response(short_response, &ocr)) {
            return false;
        }
        card_init_stage = PF_SDIO_PIO_INIT_STAGE_OPERATING_READY;
        if ((ocr & 0x80000000u) != 0u) {
            break;
        }
        sleep_ms(10u);
    } while ((uint32_t)(time_us_32() - start) < 1000000u);
    if ((ocr & 0x80000000u) == 0u) {
        return false;
    }
    card_block_addressed = supports_sd_v2 && ((ocr & 0x40000000u) != 0u);

    card_init_stage = PF_SDIO_PIO_INIT_STAGE_CID_RESPONSE;
    if (!pf_sdio_pio_host_command_bytes(&sdio_pio_host, 2u, 0u, 136u,
                                        response, sizeof(response))) {
        return false;
    }
    card_init_stage = PF_SDIO_PIO_INIT_STAGE_CID_VALIDATE;
    if (!pf_sdio_pio_validate_r2_response(response)) {
        return false;
    }
    memcpy(card_cid, response + 1u, sizeof(card_cid));
    card_init_stage = PF_SDIO_PIO_INIT_STAGE_RCA;
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
    card_init_stage = PF_SDIO_PIO_INIT_STAGE_CSD;
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
    if (!pf_sd_card_csd_transfer_hz(csd, &card_default_transfer_hz)) {
        return false;
    }

    card_init_stage = PF_SDIO_PIO_INIT_STAGE_SELECT_RESPONSE;
    if (!command_response(7u, relative_card_address, short_response, &response_argument)) {
        return false;
    }
    card_init_stage = PF_SDIO_PIO_INIT_STAGE_SELECT_READY;
    if (!wait_card_data_ready()) {
        return false;
    }
    card_init_stage = PF_SDIO_PIO_INIT_STAGE_SCR;
    if (!pf_sdio_pio_host_read_scr(&sdio_pio_host, relative_card_address, scr) ||
        !pf_sd_card_scr_supports_four_bit(scr)) {
        return false;
    }
    card_init_stage = PF_SDIO_PIO_INIT_STAGE_SELECT_STATUS;
    if (!command_response(13u, relative_card_address, short_response, &response_argument)) {
        return false;
    }
    card_init_stage = PF_SDIO_PIO_INIT_STAGE_BUS_WIDTH_PREFIX;
    if (!command_response(55u, relative_card_address, short_response, &response_argument)) {
        return false;
    }
    card_init_stage = PF_SDIO_PIO_INIT_STAGE_BUS_WIDTH;
    if (!command_response(6u, 2u, short_response, &response_argument)) {
        return false;
    }
    card_init_stage = PF_SDIO_PIO_INIT_STAGE_BLOCK_LENGTH;
    if (!card_block_addressed &&
        !command_response(16u, PF_SD_CARD_BLOCK_SIZE, short_response, &response_argument)) {
        return false;
    }

    uint32_t requested_clock = active_config.clock_hz;
    if (requested_clock == 0u) {
        requested_clock = default_clock_hz();
    }
    uint32_t transfer_clock = requested_clock;
    if (requested_clock > 25000000u) {
        uint8_t switch_status[PF_SDIO_PIO_SWITCH_STATUS_SIZE];

        if (!pf_sdio_pio_host_set_clock(&sdio_pio_host, 25000000u)) {
            return false;
        }
        if (!pf_sdio_pio_host_read_switch_status(&sdio_pio_host, 0x00fffff0u,
                                                  switch_status) ||
            !pf_sdio_pio_switch_supports_high_speed(switch_status) ||
            !pf_sdio_pio_host_read_switch_status(&sdio_pio_host, 0x80fffff1u,
                                                  switch_status) ||
            !pf_sdio_pio_switch_selected_high_speed(switch_status)) {
            transfer_clock = card_default_transfer_hz;
            if (transfer_clock > 25000000u) {
                transfer_clock = 25000000u;
            }
        }
    } else if (transfer_clock > card_default_transfer_hz) {
        transfer_clock = card_default_transfer_hz;
    }
    if (transfer_clock > *clock_limit_hz) {
        transfer_clock = *clock_limit_hz;
    }
    card_init_stage = PF_SDIO_PIO_INIT_STAGE_TRANSFER_CLOCK;
    bool clock_ready = false;
    for (size_t index = 0u;
         index < (sizeof(transfer_clock_steps_hz) / sizeof(transfer_clock_steps_hz[0]));
         index++) {
        if (transfer_clock_steps_hz[index] > transfer_clock) {
            continue;
        }
        if (!pf_sdio_pio_host_set_clock(&sdio_pio_host, transfer_clock_steps_hz[index])) {
            continue;
        }
        sdio_pio_host.data_rx_multiblock_supported = true;
        if (!probe_transfer()) {
            *clock_limit_hz = transfer_clock_steps_hz[index] - 1u;
            return false;
        }
        clock_ready = true;
        break;
    }
    if (!clock_ready) {
        return false;
    }
    card_init_stage = PF_SDIO_PIO_INIT_STAGE_NONE;
    return true;
}

pf_sdio_pio_init_stage_t pf_sdio_pio_get_init_stage(void)
{
    return card_init_stage;
}

void pf_sdio_pio_get_init_response(uint8_t response[6])
{
    if (response != NULL) {
        memcpy(response, card_init_response, sizeof(card_init_response));
    }
}

void pf_sdio_pio_get_last_short_response(uint8_t *command, uint32_t *argument,
                                         uint32_t *response_argument)
{
    if (command != NULL) {
        *command = last_short_command;
    }
    if (argument != NULL) {
        *argument = last_short_argument;
    }
    if (response_argument != NULL) {
        *response_argument = last_short_response_argument;
    }
}

void pf_sdio_pio_get_last_response_words(uint32_t words[5], size_t *word_count)
{
    if (words != NULL) {
        memcpy(words, sdio_pio_host.last_response_words,
               sizeof(sdio_pio_host.last_response_words));
    }
    if (word_count != NULL) {
        *word_count = sdio_pio_host.last_response_word_count;
    }
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

    /* A stream left open by a previous session must end before CMD0 re-initializes the card. */
    (void)pf_sdio_pio_host_write_stream_close(&sdio_pio_host);
    if (!sdio_pio_host.pio) {
        if (active_config.use_card_detect) {
            gpio_init(active_config.card_detect_gpio);
            if (active_config.card_detect_use_pull) {
                if (active_config.card_detect_pull_up) {
                    gpio_pull_up(active_config.card_detect_gpio);
                } else {
                    gpio_pull_down(active_config.card_detect_gpio);
                }
            } else {
                gpio_disable_pulls(active_config.card_detect_gpio);
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
    card_initialized = false;
    uint32_t clock_limit_hz = active_config.clock_hz;
    if (clock_limit_hz == 0u) {
        clock_limit_hz = default_clock_hz();
    }
    for (size_t attempt = 0u;
         attempt < sizeof(transfer_clock_steps_hz) / sizeof(transfer_clock_steps_hz[0]);
         attempt++) {
        uint32_t previous_limit_hz = clock_limit_hz;
        if (!pf_sdio_pio_host_set_clock(&sdio_pio_host, 400000u)) {
            break;
        }
        if (initialize_card(&clock_limit_hz)) {
            card_initialized = true;
            return PF_SD_CARD_OK;
        }
        if ((card_init_stage != PF_SDIO_PIO_INIT_STAGE_TRANSFER_CLOCK) ||
            (clock_limit_hz >= previous_limit_hz)) {
            break;
        }
    }
    return PF_SD_CARD_INIT_FAILED;
}

bool pf_sdio_pio_is_initialized(void)
{
    if (card_initialized && active_config.use_card_detect &&
        (((gpio_get(active_config.card_detect_gpio) != 0u) != active_config.card_detect_active_high))) {
        card_initialized = false;
        (void)pf_sdio_pio_host_write_stream_close(&sdio_pio_host);
    }
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

    result = pf_sd_card_validate_transfer(first_block, read_buffer != NULL ? read_buffer : write_buffer,
                                          block_count, pf_sdio_pio_block_count());
    if (result != PF_SD_CARD_OK) {
        return result;
    }

    bool continues_write = (write_buffer != NULL) &&
        pf_sdio_pio_host_write_stream_continues(&sdio_pio_host, first_block);
    if (!continues_write && !pf_sdio_pio_host_write_stream_close(&sdio_pio_host)) {
        return PF_SD_CARD_IO_ERROR;
    }
    if ((write_buffer != NULL) && (continues_write || (block_count > 1u))) {
        return pf_sdio_pio_host_write_blocks(&sdio_pio_host, first_block,
                                              card_block_addressed, write_buffer,
                                              block_count)
            ? PF_SD_CARD_OK : PF_SD_CARD_IO_ERROR;
    }
    if ((read_buffer != NULL) && (block_count > 1u)) {
        return pf_sdio_pio_host_read_blocks(&sdio_pio_host, first_block,
                                             card_block_addressed, read_buffer,
                                             block_count)
            ? PF_SD_CARD_OK : PF_SD_CARD_IO_ERROR;
    }

    while (block_count > 0u) {
        if (active_config.use_card_detect &&
            ((gpio_get(active_config.card_detect_gpio) != 0u) != active_config.card_detect_active_high)) {
            card_initialized = false;
            return PF_SD_CARD_NOT_READY;
        }
        bool transferred = (read_buffer != NULL)
            ? pf_sdio_pio_host_read_sector(&sdio_pio_host, first_block,
                                            card_block_addressed, read_buffer)
            : pf_sdio_pio_host_write_sector(&sdio_pio_host, first_block,
                                             card_block_addressed, write_buffer);
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
    if (!card_initialized) {
        return PF_SD_CARD_NOT_READY;
    }
    return pf_sdio_pio_host_write_stream_close(&sdio_pio_host)
        ? PF_SD_CARD_OK : PF_SD_CARD_IO_ERROR;
}

void pf_sdio_pio_get_info(pf_sd_card_info_t *info)
{
    if (info == NULL) {
        return;
    }
    *info = (pf_sd_card_info_t){ 0 };
    if (!configuration_set) {
        return;
    }
    if (active_config.use_card_detect) {
        info->presence_known = true;
        info->present = ((gpio_get(active_config.card_detect_gpio) != 0u) ==
                         active_config.card_detect_active_high);
    } else if (pf_sdio_pio_is_initialized()) {
        info->presence_known = true;
        info->present = true;
    }
    info->initialized = pf_sdio_pio_is_initialized();
    if (info->initialized) {
        info->cid_valid = true;
        info->bus_clock_hz = sdio_pio_host.current_clock_hz;
        info->block_count = card_sector_count;
        memcpy(info->cid, card_cid, sizeof(info->cid));
    }
}