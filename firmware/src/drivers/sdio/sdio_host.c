#include "sdio_host.h"

#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "sdio_host.pio.h"
#include "sdio_protocol.h"

#define SDIO_COMMAND_TIMEOUT_US 100000u
#define SDIO_RESPONSE_IRQ 0u
#define SDIO_DATA_READ_IRQ 1u
#define SDIO_DATA_NIBBLE_COUNT 1040u
#define SDIO_DATA_WORD_COUNT PF_SDIO_TRANSFER_WORDS

static bool load_command_program(pf_sdio_host_t *host)
{
    pio_sm_config config;

    if (!pio_can_add_program(host->pio, &pf_sdio_command_program)) {
        return false;
    }
    host->command_program_offset = pio_add_program(host->pio, &pf_sdio_command_program);
    host->command_program_loaded = true;
    config = pf_sdio_command_program_get_default_config(host->command_program_offset);
    sm_config_set_out_pins(&config, host->command_gpio, 1u);
    sm_config_set_set_pins(&config, host->command_gpio, 1u);
    sm_config_set_in_pins(&config, host->command_gpio);
    sm_config_set_jmp_pin(&config, host->command_gpio);
    sm_config_set_sideset_pins(&config, host->clock_gpio);
    sm_config_set_out_shift(&config, false, true, 32u);
    sm_config_set_in_shift(&config, false, true, 32u);
    sm_config_set_mov_status(&config, STATUS_TX_LESSTHAN, 2u);
    pio_sm_init(host->pio, host->command_sm, host->command_program_offset, &config);
    pio_sm_set_consecutive_pindirs(host->pio, host->command_sm, host->clock_gpio, 1u, true);
    return true;
}

static bool load_data_rx_program(pf_sdio_host_t *host)
{
    pio_sm_config config;

    if (!pio_can_add_program(host->pio, &pf_sdio_data_rx_program)) {
        return false;
    }
    host->data_rx_program_offset = pio_add_program(host->pio, &pf_sdio_data_rx_program);
    config = pf_sdio_data_rx_program_get_default_config(host->data_rx_program_offset);
    sm_config_set_in_pins(&config, host->data_gpio_base);
    sm_config_set_in_shift(&config, false, true, 32u);
    sm_config_set_out_shift(&config, false, false, 32u);
    pio_sm_init(host->pio, host->data_sm, host->data_rx_program_offset, &config);
    pio_sm_set_consecutive_pindirs(host->pio, host->data_sm, host->data_gpio_base, 4u, false);
    host->data_rx_program_loaded = true;
    return true;
}

static bool restore_command_mode(pf_sdio_host_t *host)
{
    pio_sm_set_enabled(host->pio, host->command_sm, false);
    pio_sm_set_enabled(host->pio, host->data_sm, false);

    if (host->data_tx_program_loaded) {
        pio_remove_program(host->pio, &pf_sdio_data_tx_program,
                           host->data_tx_program_offset);
        host->data_tx_program_loaded = false;
    }
    if (host->clock_program_loaded) {
        pio_remove_program(host->pio, &pf_sdio_clock_program,
                           host->clock_program_offset);
        host->clock_program_loaded = false;
    }

    if (!host->command_program_loaded && !load_command_program(host)) {
        return false;
    }
    if (!host->data_rx_program_loaded && !load_data_rx_program(host)) {
        return false;
    }
    pio_sm_clear_fifos(host->pio, host->command_sm);
    pio_sm_clear_fifos(host->pio, host->data_sm);
    pio_interrupt_clear(host->pio, SDIO_RESPONSE_IRQ);
    pio_sm_restart(host->pio, host->command_sm);
    pio_sm_exec(host->pio, host->command_sm,
                pio_encode_jmp(host->command_program_offset));
    pio_sm_set_clkdiv(host->pio, host->command_sm,
                      (float)host->system_clock_hz / (2.0f * (float)host->current_clock_hz));
    pio_sm_set_enabled(host->pio, host->command_sm, true);
    return true;
}

static bool wait_for_command_complete(pf_sdio_host_t *host,
                                      uint32_t *response_words,
                                      size_t response_word_count)
{
    uint32_t start = time_us_32();
    size_t words_read = 0u;

    while (!pio_interrupt_get(host->pio, SDIO_RESPONSE_IRQ) ||
           (words_read < response_word_count)) {
        while ((words_read < response_word_count) &&
               (pio_sm_get_rx_fifo_level(host->pio, host->command_sm) > 0u)) {
            response_words[words_read++] = pio_sm_get(host->pio, host->command_sm);
        }
        if ((uint32_t)(time_us_32() - start) >= SDIO_COMMAND_TIMEOUT_US) {
            return false;
        }
        tight_loop_contents();
    }

    pio_interrupt_clear(host->pio, SDIO_RESPONSE_IRQ);
    return words_read == response_word_count;
}

bool pf_sdio_host_init(pf_sdio_host_t *host,
                              const pf_sdio_config_t *config)
{
    int state_machine;
    int dma_channel;
    uint32_t data_gpio_base;

    if ((host == NULL) || (config == NULL) ||
        (((config->pio != pio0) && (config->pio != pio1))) ||
        !pio_can_add_program(config->pio, &pf_sdio_command_program)) {
        return false;
    }

    memset(host, 0, sizeof(*host));
    host->pio = config->pio;
    host->command_gpio = config->command_gpio;
    memcpy(host->data_gpio, config->data_gpio, sizeof(host->data_gpio));
    data_gpio_base = config->data_gpio[0];
    for (uint lane = 1u; lane < 4u; lane++) {
        if (config->data_gpio[lane] < data_gpio_base) {
            data_gpio_base = config->data_gpio[lane];
        }
    }
    host->data_gpio_base = data_gpio_base;
    host->clock_gpio = config->clock_gpio;
    host->system_clock_hz = clock_get_hz(clk_sys);

    state_machine = pio_claim_unused_sm(host->pio, false);
    if (state_machine < 0) {
        memset(host, 0, sizeof(*host));
        return false;
    }
    host->command_sm = (uint)state_machine;
    state_machine = pio_claim_unused_sm(host->pio, false);
    if (state_machine < 0) {
        pio_sm_unclaim(host->pio, host->command_sm);
        memset(host, 0, sizeof(*host));
        return false;
    }
    host->data_sm = (uint)state_machine;
    dma_channel = dma_claim_unused_channel(false);
    if (dma_channel < 0) {
        pio_sm_unclaim(host->pio, host->data_sm);
        pio_sm_unclaim(host->pio, host->command_sm);
        memset(host, 0, sizeof(*host));
        return false;
    }
    host->dma_channel = (uint)dma_channel;

    pio_gpio_init(host->pio, host->clock_gpio);
    pio_gpio_init(host->pio, host->command_gpio);
    for (uint pin = 0u; pin < 4u; pin++) {
        pio_gpio_init(host->pio, host->data_gpio_base + pin);
    }
    host->input_sync_bypass_mask = (1u << host->clock_gpio) |
                                   (1u << host->command_gpio);
    for (uint pin = 0u; pin < 4u; pin++) {
        host->input_sync_bypass_mask |= 1u << (host->data_gpio_base + pin);
    }
    host->previous_input_sync_bypass = host->pio->input_sync_bypass;
    host->pio->input_sync_bypass |= host->input_sync_bypass_mask;

    if (!load_command_program(host) || !load_data_rx_program(host)) {
        pf_sdio_host_deinit(host);
        return false;
    }

    gpio_pull_up(host->command_gpio);
    for (uint pin = 0u; pin < 4u; pin++) {
        gpio_pull_up(host->data_gpio_base + pin);
    }

    if (!pf_sdio_host_set_clock(host, 400000u)) {
        pf_sdio_host_deinit(host);
        return false;
    }

    pio_sm_set_enabled(host->pio, host->command_sm, true);
    return true;
}

void pf_sdio_host_deinit(pf_sdio_host_t *host)
{
    if ((host == NULL) || (host->pio == NULL)) {
        return;
    }

    pio_sm_set_enabled(host->pio, host->command_sm, false);
    pio_sm_set_enabled(host->pio, host->data_sm, false);
    if (host->data_rx_program_loaded) {
        pio_remove_program(host->pio, &pf_sdio_data_rx_program,
                           host->data_rx_program_offset);
        host->data_rx_program_loaded = false;
    }
    if (host->command_program_loaded) {
        pio_remove_program(host->pio, &pf_sdio_command_program,
                           host->command_program_offset);
        host->command_program_loaded = false;
    }
    if (host->data_tx_program_loaded) {
        pio_remove_program(host->pio, &pf_sdio_data_tx_program,
                           host->data_tx_program_offset);
    }
    if (host->clock_program_loaded) {
        pio_remove_program(host->pio, &pf_sdio_clock_program,
                           host->clock_program_offset);
    }
    host->pio->input_sync_bypass =
        (host->pio->input_sync_bypass & ~host->input_sync_bypass_mask) |
        (host->previous_input_sync_bypass & host->input_sync_bypass_mask);
    dma_channel_unclaim(host->dma_channel);
    pio_sm_unclaim(host->pio, host->data_sm);
    pio_sm_unclaim(host->pio, host->command_sm);
    memset(host, 0, sizeof(*host));
}

bool pf_sdio_host_set_clock(pf_sdio_host_t *host, uint32_t clock_hz)
{
    float divider;

    if ((host == NULL) || (host->pio == NULL) || (clock_hz == 0u) ||
        (clock_hz > host->system_clock_hz / 2u)) {
        return false;
    }

    divider = (float)host->system_clock_hz / (2.0f * (float)clock_hz);
    pio_sm_set_clkdiv(host->pio, host->command_sm, divider);
    host->current_clock_hz = clock_hz;
    return true;
}

bool pf_sdio_host_command(pf_sdio_host_t *host, uint8_t command,
                                 uint32_t argument, uint16_t response_bits,
                                 uint32_t *response_words, size_t response_word_capacity)
{
    uint8_t frame[6];
    uint32_t command_first_word;
    uint32_t command_last_word;
    size_t response_word_count = (response_bits + 31u) / 32u;

    if ((host == NULL) || (host->pio == NULL) ||
        ((response_bits != 0u) &&
         ((response_words == NULL) || (response_word_count > response_word_capacity))) ||
        (response_bits > 136u) ||
        ((response_bits != 0u) && (response_bits < 8u)) ||
        !pf_sdio_build_command(command, argument, frame)) {
        return false;
    }

    command_first_word = ((uint32_t)frame[0] << 24u) |
                         ((uint32_t)frame[1] << 16u) |
                         ((uint32_t)frame[2] << 8u) |
                         (uint32_t)frame[3];
    command_last_word = ((uint32_t)frame[4] << 24u) |
                        ((uint32_t)frame[5] << 16u);

    pio_sm_clear_fifos(host->pio, host->command_sm);
    pio_interrupt_clear(host->pio, SDIO_RESPONSE_IRQ);
    pio_sm_put_blocking(host->pio, host->command_sm, command_first_word);
    pio_sm_put_blocking(host->pio, host->command_sm, command_last_word);
    pio_sm_put_blocking(host->pio, host->command_sm,
                        (response_bits == 0u) ? 0u : ((uint32_t)(response_bits - 1u) << 24u));

    if (!wait_for_command_complete(host, response_words, response_word_count)) {
        pio_sm_clear_fifos(host->pio, host->command_sm);
        pio_sm_restart(host->pio, host->command_sm);
        pio_sm_exec(host->pio, host->command_sm,
                    pio_encode_jmp(host->command_program_offset));
        return false;
    }

    return true;
}

bool pf_sdio_host_command_bytes(pf_sdio_host_t *host, uint8_t command,
                                       uint32_t argument, uint16_t response_bits,
                                       uint8_t *response, size_t response_capacity)
{
    uint32_t response_words[5] = { 0u };
    size_t response_size = (response_bits + 7u) / 8u;
    size_t response_word_count = (response_bits + 31u) / 32u;

    if ((response_size > response_capacity) ||
        ((response_size > 0u) && (response == NULL)) ||
        (response_word_count > sizeof(response_words) / sizeof(response_words[0])) ||
        !pf_sdio_host_command(host, command, argument, response_bits,
                                     response_words, response_word_count)) {
        return false;
    }

    return pf_sdio_response_words_to_bytes(response_words, response_bits,
                                                  response, response_capacity);
}

bool pf_sdio_host_read_sector(pf_sdio_host_t *host, uint32_t sector,
                                     bool block_addressed, uint8_t block[512])
{
    uint32_t transfer_words[PF_SDIO_TRANSFER_WORDS] __attribute__((aligned(4)));
    uint8_t response[6];
    uint32_t response_argument;
    uint32_t command_argument;
    dma_channel_config dma_config;
    uint32_t start;
    bool success = false;

    if ((host == NULL) || (block == NULL) || !host->data_rx_program_loaded ||
        (!block_addressed && (sector > (UINT32_MAX / PF_SDIO_BLOCK_SIZE)))) {
        return false;
    }

    command_argument = block_addressed ? sector : sector * PF_SDIO_BLOCK_SIZE;
    pio_sm_clear_fifos(host->pio, host->data_sm);
    pio_interrupt_clear(host->pio, SDIO_DATA_READ_IRQ);
    pio_sm_put_blocking(host->pio, host->data_sm, SDIO_DATA_NIBBLE_COUNT - 1u);

    dma_config = dma_channel_get_default_config(host->dma_channel);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_config, false);
    channel_config_set_write_increment(&dma_config, true);
    channel_config_set_dreq(&dma_config, pio_get_dreq(host->pio, host->data_sm, false));
    dma_channel_configure(host->dma_channel, &dma_config, transfer_words,
                          &host->pio->rxf[host->data_sm], SDIO_DATA_WORD_COUNT, false);
    dma_channel_start(host->dma_channel);
    pio_sm_set_enabled(host->pio, host->data_sm, true);

    if (!pf_sdio_host_command_bytes(host, 17u, command_argument, 48u,
                                           response, sizeof(response)) ||
        !pf_sdio_parse_response(17u, response, &response_argument, true)) {
        goto cleanup;
    }

    start = time_us_32();
    while (!pio_interrupt_get(host->pio, SDIO_DATA_READ_IRQ) ||
           dma_channel_is_busy(host->dma_channel)) {
        if ((uint32_t)(time_us_32() - start) > 1000000u) {
            goto cleanup;
        }
        tight_loop_contents();
    }

    dma_channel_wait_for_finish_blocking(host->dma_channel);
    success = pf_sdio_remap_data_lanes(transfer_words, host->data_gpio, false) &&
              pf_sdio_unpack_data_block(transfer_words, block);

cleanup:
    pio_sm_set_enabled(host->pio, host->data_sm, false);
    if (dma_channel_is_busy(host->dma_channel)) {
        dma_channel_abort(host->dma_channel);
    }
    pio_sm_clear_fifos(host->pio, host->data_sm);
    pio_interrupt_clear(host->pio, SDIO_DATA_READ_IRQ);
    pio_sm_restart(host->pio, host->data_sm);
    pio_sm_exec(host->pio, host->data_sm,
                pio_encode_jmp(host->data_rx_program_offset));
    return success;
}

bool pf_sdio_host_write_sector(pf_sdio_host_t *host, uint32_t sector,
                                      bool block_addressed, const uint8_t block[512])
{
    uint32_t transfer_words[PF_SDIO_TRANSFER_WORDS] __attribute__((aligned(4))) = { 0u };
    uint8_t response[6];
    uint32_t response_argument;
    uint32_t command_argument;
    dma_channel_config dma_config;
    pio_sm_config state_machine_config;
    uint32_t start;
    bool success = false;

    if ((host == NULL) || (block == NULL) ||
        (!block_addressed && (sector > (UINT32_MAX / PF_SDIO_BLOCK_SIZE)))) {
        return false;
    }
    pf_sdio_pack_data_block(block, transfer_words);
    if (!pf_sdio_remap_data_lanes(transfer_words, host->data_gpio, true)) {
        return false;
    }

    command_argument = block_addressed ? sector : sector * PF_SDIO_BLOCK_SIZE;
    if (!pf_sdio_host_command_bytes(host, 24u, command_argument, 48u,
                                           response, sizeof(response)) ||
        !pf_sdio_parse_response(24u, response, &response_argument, true)) {
        return false;
    }

    pio_sm_set_enabled(host->pio, host->command_sm, false);
    pio_sm_set_enabled(host->pio, host->data_sm, false);
    if (host->data_rx_program_loaded) {
        pio_remove_program(host->pio, &pf_sdio_data_rx_program,
                           host->data_rx_program_offset);
        host->data_rx_program_loaded = false;
    }
    if (host->command_program_loaded) {
        pio_remove_program(host->pio, &pf_sdio_command_program,
                           host->command_program_offset);
        host->command_program_loaded = false;
    }

    if (!pio_can_add_program(host->pio, &pf_sdio_clock_program) ||
        !pio_can_add_program(host->pio, &pf_sdio_data_tx_program)) {
        goto restore;
    }
    host->clock_program_offset = pio_add_program(host->pio, &pf_sdio_clock_program);
    host->clock_program_loaded = true;
    host->data_tx_program_offset = pio_add_program(host->pio, &pf_sdio_data_tx_program);
    host->data_tx_program_loaded = true;

    state_machine_config = pf_sdio_clock_program_get_default_config(host->clock_program_offset);
    sm_config_set_sideset_pins(&state_machine_config, host->clock_gpio);
    pio_sm_init(host->pio, host->command_sm, host->clock_program_offset, &state_machine_config);
    pio_sm_set_consecutive_pindirs(host->pio, host->command_sm, host->clock_gpio, 1u, true);
    pio_sm_set_clkdiv(host->pio, host->command_sm,
                      (float)host->system_clock_hz / (2.0f * (float)host->current_clock_hz));

    state_machine_config = pf_sdio_data_tx_program_get_default_config(host->data_tx_program_offset);
    sm_config_set_out_pins(&state_machine_config, host->data_gpio_base, 4u);
    sm_config_set_set_pins(&state_machine_config, host->data_gpio_base, 4u);
    sm_config_set_in_pins(&state_machine_config, host->data_gpio_base);
    sm_config_set_out_shift(&state_machine_config, false, true, 32u);
    sm_config_set_in_shift(&state_machine_config, false, false, 32u);
    pio_sm_init(host->pio, host->data_sm, host->data_tx_program_offset, &state_machine_config);
    pio_sm_clear_fifos(host->pio, host->command_sm);
    pio_sm_clear_fifos(host->pio, host->data_sm);
    pio_sm_put_blocking(host->pio, host->data_sm, SDIO_DATA_NIBBLE_COUNT - 1u);
    pio_interrupt_clear(host->pio, 2u);

    dma_config = dma_channel_get_default_config(host->dma_channel);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_config, true);
    channel_config_set_write_increment(&dma_config, false);
    channel_config_set_dreq(&dma_config, pio_get_dreq(host->pio, host->data_sm, true));
    dma_channel_configure(host->dma_channel, &dma_config,
                          &host->pio->txf[host->data_sm], transfer_words,
                          SDIO_DATA_WORD_COUNT, false);
    pio_sm_set_enabled(host->pio, host->command_sm, true);
    dma_channel_start(host->dma_channel);
    pio_sm_set_enabled(host->pio, host->data_sm, true);

    start = time_us_32();
    while (!pio_interrupt_get(host->pio, 2u) || dma_channel_is_busy(host->dma_channel)) {
        if ((uint32_t)(time_us_32() - start) > 1000000u) {
            goto restore;
        }
        tight_loop_contents();
    }
    dma_channel_wait_for_finish_blocking(host->dma_channel);
    if (pio_sm_get_rx_fifo_level(host->pio, host->data_sm) > 0u) {
        success = pf_sdio_write_response_accepted(
            pio_sm_get(host->pio, host->data_sm));
    }

restore:
    if (dma_channel_is_busy(host->dma_channel)) {
        dma_channel_abort(host->dma_channel);
    }
    pio_interrupt_clear(host->pio, 2u);
    return restore_command_mode(host) && success;
}