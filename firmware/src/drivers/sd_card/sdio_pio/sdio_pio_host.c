#include "sdio_pio_host.h"
#include "sd_card_protocol.h"

#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "sd_card_block.h"
#include "sdio_pio_host.pio.h"
#include "sdio_pio_protocol.h"

#define SDIO_COMMAND_TIMEOUT_US 100000u
#define SDIO_RESPONSE_IRQ 0u
#define SDIO_DATA_READ_IRQ 1u
#define SDIO_DATA_NIBBLE_COUNT 1040u
#define SDIO_DATA_WORD_COUNT PF_SDIO_PIO_TRANSFER_WORDS
#define SDIO_READ_BATCH_BLOCKS 64u

static uint32_t read_wire_buffer[SDIO_DATA_WORD_COUNT * SDIO_READ_BATCH_BLOCKS];
static pf_sdio_pio_wait_hook_t data_wait_hook;

void pf_sdio_pio_set_wait_hook(pf_sdio_pio_wait_hook_t hook)
{
    data_wait_hook = hook;
}

static void wait_data_idle(pf_sdio_pio_host_t *host, uint32_t completion_flag)
{
    if (data_wait_hook != NULL) {
        data_wait_hook(host->pio, completion_flag);
    } else {
        pf_sd_card_idle();
    }
}

static void load_batch_count(pf_sdio_pio_host_t *host, uint32_t batch_count)
{
    pio_sm_put_blocking(host->pio, host->data_sm, batch_count - 1u);
    pio_sm_exec(host->pio, host->data_sm, pio_encode_pull(false, true));
    pio_sm_exec(host->pio, host->data_sm, pio_encode_mov(pio_y, pio_osr));
}

static void start_data_rx(pf_sdio_pio_host_t *host, uint32_t *destination,
                          uint32_t transfer_words, uint32_t nibble_count,
                          uint32_t block_count)
{
    pio_sm_set_enabled(host->pio, host->data_sm, false);
    pio_sm_clear_fifos(host->pio, host->data_sm);
    pio_interrupt_clear(host->pio, SDIO_DATA_READ_IRQ);
    pio_sm_restart(host->pio, host->data_sm);
    pio_sm_exec(host->pio, host->data_sm,
                pio_encode_jmp(host->data_rx_program_offset));
    if (block_count > 1u) {
        load_batch_count(host, block_count);
    } else {
        pio_sm_exec(host->pio, host->data_sm, pio_encode_set(pio_y, 0u));
    }
    pio_sm_put_blocking(host->pio, host->data_sm, nibble_count - 1u);

    dma_channel_config dma_config = dma_channel_get_default_config(host->dma_channel);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
    channel_config_set_bswap(&dma_config, true);
    channel_config_set_read_increment(&dma_config, false);
    channel_config_set_write_increment(&dma_config, true);
    channel_config_set_dreq(&dma_config, pio_get_dreq(host->pio, host->data_sm, false));
    dma_channel_configure(host->dma_channel, &dma_config, destination,
                          &host->pio->rxf[host->data_sm], transfer_words, false);
    dma_channel_start(host->dma_channel);
    pio_sm_set_enabled(host->pio, host->data_sm, true);
}

static bool wait_data_rx(pf_sdio_pio_host_t *host, uint32_t timeout_us)
{
    uint32_t start = time_us_32();

    while (!pio_interrupt_get(host->pio, SDIO_DATA_READ_IRQ) ||
           dma_channel_is_busy(host->dma_channel)) {
        if ((uint32_t)(time_us_32() - start) > timeout_us) {
            pf_sd_card_profile_event(PF_SD_CARD_PROFILE_TIMEOUT);
            return false;
        }
        wait_data_idle(host, SDIO_DATA_READ_IRQ);
    }
    dma_channel_wait_for_finish_blocking(host->dma_channel);
    return true;
}

static void cleanup_data_rx(pf_sdio_pio_host_t *host)
{
    pio_sm_set_enabled(host->pio, host->data_sm, false);
    if (dma_channel_is_busy(host->dma_channel)) {
        dma_channel_abort(host->dma_channel);
    }
    pio_sm_clear_fifos(host->pio, host->data_sm);
    pio_interrupt_clear(host->pio, SDIO_DATA_READ_IRQ);
    pio_sm_restart(host->pio, host->data_sm);
    pio_sm_exec(host->pio, host->data_sm,
                pio_encode_jmp(host->data_rx_program_offset));
}

static bool read_single_blocks(pf_sdio_pio_host_t *host, uint32_t first_sector,
                               bool block_addressed, uint8_t *blocks,
                               uint32_t block_count)
{
    while (block_count > 0u) {
        if (!pf_sdio_pio_host_read_sector(host, first_sector, block_addressed, blocks)) {
            return false;
        }
        ++first_sector;
        if (blocks != NULL) {
            blocks += PF_SD_CARD_BLOCK_SIZE;
        }
        --block_count;
    }
    return true;
}

static bool decode_completed_blocks(pf_sdio_pio_host_t *host, uint8_t *blocks,
                                    uint32_t batch_words, uint32_t *decoded_count)
{
    uint32_t write_addr = dma_channel_hw_addr(host->dma_channel)->write_addr;
    uint32_t buffer_start = (uint32_t)(uintptr_t)read_wire_buffer;
    uint32_t completed;

    if ((write_addr < buffer_start) ||
        (write_addr > buffer_start + batch_words * (uint32_t)sizeof(uint32_t)) ||
        ((write_addr & (sizeof(uint32_t) - 1u)) != 0u)) {
        return false;
    }
    completed = (write_addr - buffer_start) / sizeof(uint32_t) / SDIO_DATA_WORD_COUNT;
    __dmb();
    while (*decoded_count < completed) {
        uint8_t *image = (uint8_t *)&read_wire_buffer[
            *decoded_count * SDIO_DATA_WORD_COUNT];
        bool valid = blocks != NULL
            ? pf_sdio_pio_decode_data_block(image,
                blocks + *decoded_count * PF_SD_CARD_BLOCK_SIZE)
            : pf_sdio_pio_verify_data_block(image);
        if (!valid) {
            return false;
        }
        ++*decoded_count;
    }
    return true;
}
/* ponytail: synchronous batched DMA into one bounded buffer (ceiling 32 KiB of
 * payload, 64 sectors); completed wire blocks are decoded while DMA still
 * receives later blocks. No ping-pong or second DMA channel. Upgrade path: for
 * larger batches or fully overlapped decode, add a second buffer with chained
 * DMA channels (ping-pong) instead of raising this ceiling. */
static bool load_command_program(pf_sdio_pio_host_t *host)
{
    pio_sm_config config;

    if (!pio_can_add_program(host->pio, &pf_sdio_pio_command_program)) {
        return false;
    }
    host->command_program_offset = pio_add_program(host->pio, &pf_sdio_pio_command_program);
    host->command_program_loaded = true;
    config = pf_sdio_pio_command_program_get_default_config(host->command_program_offset);
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

static const pio_program_t *data_rx_program(const pf_sdio_pio_host_t *host)
{
    return host->data_rx_fast ? &pf_sdio_pio_data_rx_fast_program
                             : &pf_sdio_pio_data_rx_program;
}

static const pio_program_t *data_tx_program(const pf_sdio_pio_host_t *host)
{
    return host->data_tx_fast ? &pf_sdio_pio_data_tx_fast_program
                              : &pf_sdio_pio_data_tx_program;
}

static bool load_data_rx_program(pf_sdio_pio_host_t *host)
{
    pio_sm_config config;

    host->data_rx_fast = host->current_clock_hz > 25000000u;
    if (!pio_can_add_program(host->pio, data_rx_program(host))) {
        return false;
    }
    host->data_rx_program_offset = pio_add_program(host->pio, data_rx_program(host));
    config = host->data_rx_fast
                 ? pf_sdio_pio_data_rx_fast_program_get_default_config(host->data_rx_program_offset)
                 : pf_sdio_pio_data_rx_program_get_default_config(host->data_rx_program_offset);
    if (host->data_rx_fast) {
        sm_config_set_clkdiv_int_frac(&config, (uint16_t)(host->clock_div_ticks >> 9u),
                                      (uint8_t)((host->clock_div_ticks >> 1u) & 0xffu));
    }
    sm_config_set_in_pins(&config, host->data_gpio_base);
    sm_config_set_in_shift(&config, false, true, 32u);
    sm_config_set_out_shift(&config, false, false, 32u);
    pio_sm_init(host->pio, host->data_sm, host->data_rx_program_offset, &config);
    pio_sm_set_consecutive_pindirs(host->pio, host->data_sm, host->data_gpio_base, 4u, false);
    host->data_rx_program_loaded = true;
    return true;
}

static bool load_tx_programs(pf_sdio_pio_host_t *host)
{
    pio_sm_config config;

    pio_sm_set_enabled(host->pio, host->command_sm, false);
    pio_sm_set_enabled(host->pio, host->data_sm, false);
    if (host->data_rx_program_loaded) {
        pio_remove_program(host->pio, data_rx_program(host), host->data_rx_program_offset);
        host->data_rx_program_loaded = false;
    }
    if (host->command_program_loaded) {
        pio_remove_program(host->pio, &pf_sdio_pio_command_program,
                           host->command_program_offset);
        host->command_program_loaded = false;
    }

    host->data_tx_fast = host->current_clock_hz > 25000000u;
    if (!pio_can_add_program(host->pio, &pf_sdio_pio_clock_program) ||
        !pio_can_add_program(host->pio, data_tx_program(host))) {
        return false;
    }
    host->clock_program_offset = pio_add_program(host->pio, &pf_sdio_pio_clock_program);
    host->clock_program_loaded = true;
    host->data_tx_program_offset = pio_add_program(host->pio, data_tx_program(host));
    host->data_tx_program_loaded = true;

    config = pf_sdio_pio_clock_program_get_default_config(host->clock_program_offset);
    sm_config_set_sideset_pins(&config, host->clock_gpio);
    pio_sm_init(host->pio, host->command_sm, host->clock_program_offset, &config);
    pio_sm_set_consecutive_pindirs(host->pio, host->command_sm, host->clock_gpio, 1u, true);
    pio_sm_set_clkdiv_int_frac(host->pio, host->command_sm,
                               (uint16_t)(host->clock_div_ticks >> 8u),
                               (uint8_t)(host->clock_div_ticks & 0xffu));

    config = host->data_tx_fast
                 ? pf_sdio_pio_data_tx_fast_program_get_default_config(host->data_tx_program_offset)
                 : pf_sdio_pio_data_tx_program_get_default_config(host->data_tx_program_offset);
    if (host->data_tx_fast) {
        sm_config_set_clkdiv_int_frac(&config, (uint16_t)(host->clock_div_ticks >> 9u),
                                      (uint8_t)((host->clock_div_ticks >> 1u) & 0xffu));
    }
    sm_config_set_out_pins(&config, host->data_gpio_base, 4u);
    sm_config_set_set_pins(&config, host->data_gpio_base, 4u);
    sm_config_set_in_pins(&config, host->data_gpio_base);
    sm_config_set_out_shift(&config, false, true, 32u);
    sm_config_set_in_shift(&config, false, false, 32u);
    pio_sm_init(host->pio, host->data_sm, host->data_tx_program_offset, &config);
    pio_sm_clear_fifos(host->pio, host->command_sm);
    pio_sm_clear_fifos(host->pio, host->data_sm);
    return true;
}

/* The fast TX program is cycle-locked to the clock, so both state machines restart together. */
static void start_tx_data(pf_sdio_pio_host_t *host)
{
    if (!host->data_tx_fast) {
        pio_sm_set_enabled(host->pio, host->data_sm, true);
        return;
    }
    pio_sm_set_enabled(host->pio, host->command_sm, false);
    pio_sm_exec(host->pio, host->command_sm, pio_encode_jmp(host->clock_program_offset));
    pio_enable_sm_mask_in_sync(host->pio, (1u << host->command_sm) | (1u << host->data_sm));
}

static bool restore_command_mode(pf_sdio_pio_host_t *host)
{
    host->mode_restore_failed = true;
    pio_sm_set_enabled(host->pio, host->command_sm, false);
    pio_sm_set_enabled(host->pio, host->data_sm, false);

    if (host->data_tx_program_loaded) {
        pio_remove_program(host->pio, data_tx_program(host),
                           host->data_tx_program_offset);
        host->data_tx_program_loaded = false;
    }
    if (host->clock_program_loaded) {
        pio_remove_program(host->pio, &pf_sdio_pio_clock_program,
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
    pio_sm_set_clkdiv_int_frac(host->pio, host->command_sm,
                               (uint16_t)(host->clock_div_ticks >> 8u),
                               (uint8_t)(host->clock_div_ticks & 0xffu));
    pio_sm_set_enabled(host->pio, host->command_sm, true);
    host->mode_restore_failed = false;
    return true;
}

static bool wait_for_command_complete(pf_sdio_pio_host_t *host,
                                      uint32_t *response_words,
                                      size_t response_word_count)
{
    uint32_t start = time_us_32();
    size_t words_read = 0u;

    host->last_response_word_count = 0u;
    memset(host->last_response_words, 0, sizeof(host->last_response_words));

    while (!pio_interrupt_get(host->pio, SDIO_RESPONSE_IRQ) ||
           (words_read < response_word_count)) {
        while ((words_read < response_word_count) &&
               (pio_sm_get_rx_fifo_level(host->pio, host->command_sm) > 0u)) {
            response_words[words_read++] = pio_sm_get(host->pio, host->command_sm);
            host->last_response_words[words_read - 1u] = response_words[words_read - 1u];
            host->last_response_word_count = words_read;
        }
        /* A preempted poller must not fail a command that already completed. */
        if (((uint32_t)(time_us_32() - start) >= SDIO_COMMAND_TIMEOUT_US) &&
            (!pio_interrupt_get(host->pio, SDIO_RESPONSE_IRQ) ||
             (words_read < response_word_count))) {
            pf_sd_card_profile_event(PF_SD_CARD_PROFILE_TIMEOUT);
            return false;
        }
        tight_loop_contents();
    }

    pio_interrupt_clear(host->pio, SDIO_RESPONSE_IRQ);
    return words_read == response_word_count;
}

bool pf_sdio_pio_host_init(pf_sdio_pio_host_t *host,
                              const pf_sdio_pio_config_t *config)
{
    int state_machine;
    int dma_channel;
    uint32_t data_gpio_base;

    if ((host == NULL) || (host->pio != NULL) || (config == NULL) ||
        (((config->pio != pio0) && (config->pio != pio1))) ||
        !pio_can_add_program(config->pio, &pf_sdio_pio_command_program)) {
        return false;
    }

    pf_sd_card_profile_set_clock(time_us_32);
    memset(host, 0, sizeof(*host));
    host->pio = config->pio;
    host->command_gpio = config->command_gpio;
    host->data0_gpio = config->data_gpio[0];
    host->data_rx_multiblock_supported = true;
    data_gpio_base = config->data_gpio[0];
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
        pf_sdio_pio_host_deinit(host);
        return false;
    }

    gpio_pull_up(host->command_gpio);
    for (uint pin = 0u; pin < 4u; pin++) {
        gpio_pull_up(host->data_gpio_base + pin);
    }

    if (!pf_sdio_pio_host_set_clock(host, 400000u)) {
        pf_sdio_pio_host_deinit(host);
        return false;
    }

    pio_sm_set_enabled(host->pio, host->command_sm, true);
    return true;
}

void pf_sdio_pio_host_deinit(pf_sdio_pio_host_t *host)
{
    if ((host == NULL) || (host->pio == NULL)) {
        return;
    }

    pio_sm_set_enabled(host->pio, host->command_sm, false);
    pio_sm_set_enabled(host->pio, host->data_sm, false);
    if (host->data_rx_program_loaded) {
        pio_remove_program(host->pio, data_rx_program(host),
                           host->data_rx_program_offset);
        host->data_rx_program_loaded = false;
    }
    if (host->command_program_loaded) {
        pio_remove_program(host->pio, &pf_sdio_pio_command_program,
                           host->command_program_offset);
        host->command_program_loaded = false;
    }
    if (host->data_tx_program_loaded) {
        pio_remove_program(host->pio, data_tx_program(host),
                           host->data_tx_program_offset);
    }
    if (host->clock_program_loaded) {
        pio_remove_program(host->pio, &pf_sdio_pio_clock_program,
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

bool pf_sdio_pio_host_set_clock(pf_sdio_pio_host_t *host, uint32_t clock_hz)
{
    uint32_t divider_ticks;

    if ((host == NULL) || (host->pio == NULL) || (clock_hz == 0u) ||
        (clock_hz > host->system_clock_hz / 4u)) {
        return false;
    }

    if (clock_hz > 25000000u) {
        /* The data state machines run at half the clock divider; equal fractions keep them in lockstep. */
        divider_ticks = 2u * (uint32_t)(((uint64_t)host->system_clock_hz * 64u +
                                         clock_hz - 1u) / clock_hz);
    } else {
        /* Slow-mode programs need whole system cycles; round the divider up. */
        divider_ticks = (uint32_t)(((uint64_t)host->system_clock_hz + 2u * clock_hz - 1u) /
                                   (2u * clock_hz)) * 256u;
    }
    host->clock_div_ticks = divider_ticks;
    pio_sm_set_clkdiv_int_frac(host->pio, host->command_sm,
                               (uint16_t)(divider_ticks >> 8u),
                               (uint8_t)(divider_ticks & 0xffu));
    host->current_clock_hz = (uint32_t)(((uint64_t)host->system_clock_hz * 128u) /
                                        divider_ticks);
    if (host->command_program_loaded) {
        pio_sm_set_enabled(host->pio, host->data_sm, false);
        if (host->data_rx_program_loaded) {
            pio_remove_program(host->pio, data_rx_program(host), host->data_rx_program_offset);
        }
        host->data_rx_program_loaded = false;
        return load_data_rx_program(host);
    }
    return true;
}

bool pf_sdio_pio_host_command(pf_sdio_pio_host_t *host, uint8_t command,
                                 uint32_t argument, uint16_t response_bits,
                                 uint32_t *response_words, size_t response_word_capacity)
{
    uint8_t frame[6];
    uint64_t frame_bits;
    uint32_t command_first_word;
    uint32_t command_last_word;
    size_t response_word_count = (response_bits + 31u) / 32u;

    if ((host == NULL) || (host->pio == NULL) || host->mode_restore_failed ||
        (host->read_stream_unresolved && (command != 12u)) ||
        ((response_bits != 0u) &&
         ((response_words == NULL) || (response_word_count > response_word_capacity))) ||
        (response_bits > 136u) ||
        ((response_bits != 0u) && (response_bits < 8u)) ||
        !pf_sdio_pio_build_command(command, argument, frame)) {
        return false;
    }

    /* The PIO program shifts a 6-bit loop count (47) before the 48 frame bits. */
    frame_bits = ((uint64_t)frame[0] << 40u) | ((uint64_t)frame[1] << 32u) |
                 ((uint64_t)frame[2] << 24u) | ((uint64_t)frame[3] << 16u) |
                 ((uint64_t)frame[4] << 8u) | (uint64_t)frame[5];
    command_first_word = (47u << 26u) | (uint32_t)(frame_bits >> 22u);
    command_last_word = (uint32_t)(frame_bits & 0x3fffffu) << 10u;

    pio_sm_clear_fifos(host->pio, host->command_sm);
    pio_interrupt_clear(host->pio, SDIO_RESPONSE_IRQ);
    pio_sm_put_blocking(host->pio, host->command_sm, command_first_word);
    pio_sm_put_blocking(host->pio, host->command_sm, command_last_word);
    pio_sm_put_blocking(host->pio, host->command_sm,
                        (response_bits == 0u) ? 0u : ((uint32_t)(response_bits - 2u) << 24u));

    uint32_t profile_start = pf_sd_card_profile_begin();
    bool command_ok = wait_for_command_complete(host, response_words, response_word_count);
    pf_sd_card_profile_end(PF_SD_CARD_PROFILE_COMMAND, profile_start);
    if (!command_ok) {
        pio_sm_clear_fifos(host->pio, host->command_sm);
        pio_sm_restart(host->pio, host->command_sm);
        pio_sm_exec(host->pio, host->command_sm,
                    pio_encode_jmp(host->command_program_offset));
        return false;
    }

    sleep_us((8000000u + host->current_clock_hz - 1u) / host->current_clock_hz);
    return true;
}

bool pf_sdio_pio_host_command_bytes(pf_sdio_pio_host_t *host, uint8_t command,
                                       uint32_t argument, uint16_t response_bits,
                                       uint8_t *response, size_t response_capacity)
{
    uint32_t response_words[5] = { 0u };
    size_t response_size = (response_bits + 7u) / 8u;
    size_t response_word_count = (response_bits + 31u) / 32u;

    if ((response_size > response_capacity) ||
        ((response_size > 0u) && (response == NULL)) ||
        (response_word_count > sizeof(response_words) / sizeof(response_words[0])) ||
        !pf_sdio_pio_host_command(host, command, argument, response_bits,
                                     response_words, response_word_count)) {
        return false;
    }

    return pf_sdio_pio_response_words_to_bytes(response_words, response_bits,
                                               response, response_capacity);
}

static bool read_data_block(pf_sdio_pio_host_t *host, uint8_t command,
                            uint32_t argument, uint8_t *data, size_t data_size)
{
    uint32_t image[PF_SDIO_PIO_TRANSFER_WORDS];
    uint8_t response[6];
    uint32_t response_argument;
    uint32_t wire_bytes;
    bool success = false;

    if ((host == NULL) || ((data == NULL) && (data_size != PF_SD_CARD_BLOCK_SIZE)) ||
        (data_size == 0u) ||
        (data_size > PF_SD_CARD_BLOCK_SIZE) || ((data_size & 3u) != 0u) ||
        !host->data_rx_program_loaded || host->read_stream_unresolved) {
        return false;
    }
    wire_bytes = (uint32_t)data_size + 8u;

    start_data_rx(host, image, wire_bytes / 4u, wire_bytes * 2u, 1u);

    if (!pf_sdio_pio_host_command_bytes(host, command, argument, 48u,
                                         response, sizeof(response)) ||
        !pf_sdio_pio_parse_response(command, response, &response_argument, true) ||
        ((response_argument & 0xfdffe008u) != 0u)) {
        goto cleanup;
    }

    if (!wait_data_rx(host, 1000000u)) {
        goto cleanup;
    }
    success = data != NULL
        ? pf_sdio_pio_decode_data_block_sized((const uint8_t *)image, data, data_size)
        : pf_sdio_pio_verify_data_block((const uint8_t *)image);

cleanup:
    cleanup_data_rx(host);
    return success;
}

bool pf_sdio_pio_host_read_sector(pf_sdio_pio_host_t *host, uint32_t sector,
                                     bool block_addressed, uint8_t block[512])
{
    uint32_t argument;

    return pf_sd_card_block_argument(sector, block_addressed, &argument) &&
           read_data_block(host, 17u, argument, block, PF_SD_CARD_BLOCK_SIZE);
}

bool pf_sdio_pio_host_read_scr(pf_sdio_pio_host_t *host, uint32_t relative_card_address,
                               uint8_t scr[8])
{
    enum { SCR_DATA_BITS = 80u, SCR_DMA_WORDS = SCR_DATA_BITS / 8u };
    uint32_t image[SCR_DMA_WORDS];
    uint8_t response[6];
    uint32_t response_argument;
    uint16_t received_crc = 0u;
    bool success = false;

    if ((host == NULL) || (scr == NULL) || !host->data_rx_program_loaded) {
        return false;
    }

    start_data_rx(host, image, SCR_DMA_WORDS, SCR_DATA_BITS, 1u);

    if (!pf_sdio_pio_host_command_bytes(host, 55u, relative_card_address, 48u,
                                         response, sizeof(response))) {
        goto cleanup;
    }
    if (!pf_sdio_pio_parse_response(55u, response, &response_argument, true) ||
        ((response_argument & 0xfdffe008u) != 0u)) {
        goto cleanup;
    }
    if (!pf_sdio_pio_host_command_bytes(host, 51u, 0u, 48u,
                                         response, sizeof(response))) {
        goto cleanup;
    }
    if (!pf_sdio_pio_parse_response(51u, response, &response_argument, true) ||
        ((response_argument & 0xfdffe008u) != 0u)) {
        goto cleanup;
    }

    if (!wait_data_rx(host, 1000000u)) {
        goto cleanup;
    }
    memset(scr, 0, 8u);
    for (size_t bit_index = 0u; bit_index < SCR_DATA_BITS; bit_index++) {
        uint8_t sample_byte = ((const uint8_t *)image)[bit_index / 2u];
        uint8_t sample_nibble = (bit_index & 1u) == 0u
            ? (uint8_t)(sample_byte >> 4u) : (uint8_t)(sample_byte & 0x0fu);
        uint8_t bit = sample_nibble & 1u;

        if (bit_index < 64u) {
            scr[bit_index / 8u] = (uint8_t)((scr[bit_index / 8u] << 1u) | bit);
        } else {
            received_crc = (uint16_t)((received_crc << 1u) | bit);
        }
    }
    success = received_crc == pf_sd_card_crc16(scr, 8u);
cleanup:
    cleanup_data_rx(host);
    return success;
}

bool pf_sdio_pio_host_read_switch_status(
    pf_sdio_pio_host_t *host, uint32_t argument,
    uint8_t status[PF_SDIO_PIO_SWITCH_STATUS_SIZE])
{
    return read_data_block(host, 6u, argument, status,
                           PF_SDIO_PIO_SWITCH_STATUS_SIZE);
}

static bool stop_multiblock_transfer(pf_sdio_pio_host_t *host)
{
    uint8_t response[6];
    uint32_t response_argument;
    uint32_t start;

    if (!pf_sdio_pio_host_command_bytes(host, 12u, 0u, 48u,
                                         response, sizeof(response))) {
        return false;
    }
    if (!pf_sdio_pio_parse_response(12u, response, &response_argument, true)) {
        return false;
    }
    if ((response_argument & 0xfdffe008u) != 0u) {
        return false;
    }

    start = time_us_32();
    uint32_t profile_start = pf_sd_card_profile_begin();
    while (gpio_get(host->data0_gpio) == 0u) {
        if ((uint32_t)(time_us_32() - start) > 1000000u) {
            pf_sd_card_profile_end(PF_SD_CARD_PROFILE_BUSY, profile_start);
            pf_sd_card_profile_event(PF_SD_CARD_PROFILE_TIMEOUT);
            return false;
        }
        pf_sd_card_idle();
    }
    pf_sd_card_profile_end(PF_SD_CARD_PROFILE_BUSY, profile_start);
    return true;
}

static bool stop_multiblock_with_clock_fallback(pf_sdio_pio_host_t *host)
{
    static const uint32_t stop_clock_steps_hz[] = {
        25000000u, 20000000u, 10000000u, 5000000u, 1000000u,
    };

    uint32_t profile_start = pf_sd_card_profile_begin();
    if (stop_multiblock_transfer(host)) {
        pf_sd_card_profile_end(PF_SD_CARD_PROFILE_STOP, profile_start);
        return true;
    }
    for (size_t index = 0u;
         index < (sizeof(stop_clock_steps_hz) / sizeof(stop_clock_steps_hz[0]));
         index++) {
        if ((stop_clock_steps_hz[index] >= host->current_clock_hz) ||
            !pf_sdio_pio_host_set_clock(host, stop_clock_steps_hz[index])) {
            continue;
        }
        if (stop_multiblock_transfer(host)) {
            pf_sd_card_profile_end(PF_SD_CARD_PROFILE_STOP, profile_start);
            return true;
        }
    }
    pf_sd_card_profile_end(PF_SD_CARD_PROFILE_STOP, profile_start);
    return false;
}

bool pf_sdio_pio_host_read_blocks(pf_sdio_pio_host_t *host, uint32_t first_sector,
                                      bool block_addressed, uint8_t *blocks,
                                      uint32_t block_count)
{
    if ((host == NULL) || (host->pio == NULL) ||
        !host->data_rx_program_loaded || host->read_stream_unresolved) {
        return false;
    }
    if (block_count == 0u) {
        return true;
    }
    if (!host->data_rx_multiblock_supported || (block_count == 1u)) {
        return read_single_blocks(host, first_sector, block_addressed, blocks,
                                  block_count);
    }

    while (block_count > 0u) {
        uint32_t command_argument;
        uint32_t response_argument;
        uint8_t response[6];
        uint32_t batch_count = block_count;
        uint32_t batch_words;
        uint32_t start;
        bool command_ok;
        bool transfer_ok = false;
        bool crc_failed = false;
        bool timed_out = false;
        uint32_t decoded_count = 0u;

        if (batch_count > SDIO_READ_BATCH_BLOCKS) {
            batch_count = SDIO_READ_BATCH_BLOCKS;
        }
        if (!pf_sd_card_block_argument(first_sector, block_addressed,
                                       &command_argument)) {
            return false;
        }
        batch_words = SDIO_DATA_WORD_COUNT * batch_count;
        start_data_rx(host, read_wire_buffer, batch_words, SDIO_DATA_NIBBLE_COUNT,
                      batch_count);

        /* CMD18 may reach the wire even if its response fails. Stop every attempt. */
        command_ok = pf_sdio_pio_host_command_bytes(host, 18u, command_argument,
                                                     48u, response, sizeof(response));
        host->read_stream_unresolved = true;
        bool response_valid = command_ok &&
            pf_sdio_pio_parse_response(18u, response, &response_argument, true);
        bool response_ok = response_valid &&
            ((response_argument & 0xfdffe008u) == 0u);
        if (response_ok) {
            start = time_us_32();
            uint32_t profile_start = pf_sd_card_profile_begin();
            while (!pio_interrupt_get(host->pio, SDIO_DATA_READ_IRQ) ||
                   dma_channel_is_busy(host->dma_channel)) {
                if ((uint32_t)(time_us_32() - start) > 1000000u) {
                    timed_out = true;
                    pf_sd_card_profile_event(PF_SD_CARD_PROFILE_TIMEOUT);
                    break;
                }
                if (!crc_failed &&
                    !decode_completed_blocks(host, blocks, batch_words, &decoded_count)) {
                    crc_failed = true;
                }
                wait_data_idle(host, SDIO_DATA_READ_IRQ);
            }
            pf_sd_card_profile_end(PF_SD_CARD_PROFILE_DATA, profile_start);
            transfer_ok = pio_interrupt_get(host->pio, SDIO_DATA_READ_IRQ) &&
                          !dma_channel_is_busy(host->dma_channel);
            if (transfer_ok && !crc_failed &&
                (!decode_completed_blocks(host, blocks, batch_words, &decoded_count) ||
                 (decoded_count != batch_count))) {
                crc_failed = true;
            }
        }

        cleanup_data_rx(host);
        bool stop_ok = stop_multiblock_with_clock_fallback(host);
        host->read_stream_unresolved = !stop_ok;
        if (!stop_ok || !response_valid || crc_failed || timed_out) {
            return false;
        }
        if (!transfer_ok) {
            pf_sd_card_profile_event(PF_SD_CARD_PROFILE_BATCH_FALLBACK);
            host->data_rx_multiblock_supported = false;
            return read_single_blocks(host, first_sector, block_addressed, blocks,
                                      block_count);
        }

        if (blocks != NULL) {
            blocks += batch_count * PF_SD_CARD_BLOCK_SIZE;
        }
        first_sector += batch_count;
        block_count -= batch_count;
    }
    return true;
}

bool pf_sdio_pio_host_write_sector(pf_sdio_pio_host_t *host, uint32_t sector,
                                      bool block_addressed, const uint8_t block[512])
{
    uint32_t image[PF_SDIO_PIO_TRANSFER_WORDS];
    uint8_t response[6];
    uint32_t response_argument;
    uint32_t command_argument;
    dma_channel_config dma_config;
    uint32_t start;
    bool success = false;

    if ((host == NULL) || (block == NULL) ||
        !pf_sd_card_block_argument(sector, block_addressed, &command_argument)) {
        return false;
    }
    if (!pf_sdio_pio_encode_data_block(block, (uint8_t *)image)) {
        return false;
    }

    if (!pf_sdio_pio_host_command_bytes(host, 24u, command_argument, 48u,
                                           response, sizeof(response)) ||
        !pf_sdio_pio_parse_response(24u, response, &response_argument, true) ||
        ((response_argument & 0xfdffe008u) != 0u)) {
        return false;
    }

    if (!load_tx_programs(host)) {
        goto restore;
    }
    pio_sm_put_blocking(host->pio, host->data_sm, SDIO_DATA_NIBBLE_COUNT - 1u);
    pio_interrupt_clear(host->pio, 2u);

    dma_config = dma_channel_get_default_config(host->dma_channel);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
    channel_config_set_bswap(&dma_config, true);
    channel_config_set_read_increment(&dma_config, true);
    channel_config_set_write_increment(&dma_config, false);
    channel_config_set_dreq(&dma_config, pio_get_dreq(host->pio, host->data_sm, true));
    dma_channel_configure(host->dma_channel, &dma_config,
                          &host->pio->txf[host->data_sm], image,
                          SDIO_DATA_WORD_COUNT, false);
    pio_sm_set_enabled(host->pio, host->command_sm, true);
    dma_channel_start(host->dma_channel);
    start_tx_data(host);

    start = time_us_32();
    while (!pio_interrupt_get(host->pio, 2u) || dma_channel_is_busy(host->dma_channel)) {
        if ((uint32_t)(time_us_32() - start) > 1000000u) {
            goto restore;
        }
        wait_data_idle(host, 2u);
    }
    dma_channel_wait_for_finish_blocking(host->dma_channel);
    if (pio_sm_get_rx_fifo_level(host->pio, host->data_sm) > 0u) {
        success = pf_sdio_pio_write_response_accepted(pio_sm_get(host->pio, host->data_sm));
    }

restore:
    if (dma_channel_is_busy(host->dma_channel)) {
        dma_channel_abort(host->dma_channel);
    }
    pio_interrupt_clear(host->pio, 2u);
    return restore_command_mode(host) && success;
}

bool pf_sdio_pio_host_write_stream_continues(const pf_sdio_pio_host_t *host, uint32_t sector)
{
    return (host != NULL) && host->write_stream_open &&
           (host->write_stream_next_sector == sector);
}

bool pf_sdio_pio_host_recover(pf_sdio_pio_host_t *host)
{
    if ((host == NULL) || (!host->write_stream_open && !host->read_stream_unresolved &&
                            !host->mode_restore_failed)) {
        return true;
    }
    if (!restore_command_mode(host)) {
        return false;
    }
    if ((host->write_stream_open || host->read_stream_unresolved) &&
        !stop_multiblock_with_clock_fallback(host)) {
        return false;
    }
    host->write_stream_open = false;
    host->read_stream_unresolved = false;
    return true;
}

void pf_sdio_pio_host_forget_removed_card(pf_sdio_pio_host_t *host)
{
    if ((host == NULL) || (host->pio == NULL)) {
        return;
    }
    host->write_stream_open = false;
    host->read_stream_unresolved = false;
    if (!restore_command_mode(host)) {
        pf_sdio_pio_host_deinit(host);
    }
}

bool pf_sdio_pio_host_write_blocks(pf_sdio_pio_host_t *host, uint32_t first_sector,
                                      bool block_addressed, const uint8_t *blocks,
                                      uint32_t block_count)
{
    uint32_t images[2][PF_SDIO_PIO_TRANSFER_WORDS];
    uint32_t command_argument;
    uint32_t response_argument;
    uint8_t response[6];
    bool transfer_ok = true;
    uint32_t start;
    bool continues = pf_sdio_pio_host_write_stream_continues(host, first_sector);

    if ((host == NULL) || (host->pio == NULL) || (blocks == NULL) ||
        (block_count == 0u) || (!continues && (block_count < 2u)) ||
        !pf_sd_card_block_argument(first_sector, block_addressed, &command_argument)) {
        return false;
    }
    if (host->write_stream_open && !continues &&
        !pf_sdio_pio_host_recover(host)) {
        return false;
    }
    if (!pf_sdio_pio_encode_data_block(blocks, (uint8_t *)images[0])) {
        return false;
    }
    if (!continues) {
        bool command_ok = pf_sdio_pio_host_command_bytes(
            host, 25u, command_argument, 48u, response, sizeof(response));
        bool response_valid = command_ok &&
            pf_sdio_pio_parse_response(25u, response, &response_argument, true);
        if (!response_valid) {
            host->write_stream_open = true;
            host->write_stream_next_sector = first_sector;
            (void)pf_sdio_pio_host_recover(host);
            return false;
        }
        if ((response_argument & 0xfdffe008u) != 0u) {
            return false;
        }
        host->write_stream_open = true;
        host->write_stream_next_sector = first_sector;

        if (!load_tx_programs(host)) {
            (void)pf_sdio_pio_host_recover(host);
            return false;
        }
        pio_sm_set_enabled(host->pio, host->command_sm, true);
    }
    for (uint32_t block_index = 0u; block_index < block_count; block_index++) {
        uint32_t *image = images[block_index & 1u];
        uint32_t response_word = 0u;
        dma_channel_config dma_config;

        pio_sm_clear_fifos(host->pio, host->data_sm);
        pio_interrupt_clear(host->pio, 2u);
        pio_sm_restart(host->pio, host->data_sm);
        pio_sm_exec(host->pio, host->data_sm,
                    pio_encode_jmp(host->data_tx_program_offset));
        pio_sm_put_blocking(host->pio, host->data_sm, SDIO_DATA_NIBBLE_COUNT - 1u);

        dma_config = dma_channel_get_default_config(host->dma_channel);
        channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
        channel_config_set_bswap(&dma_config, true);
        channel_config_set_read_increment(&dma_config, true);
        channel_config_set_write_increment(&dma_config, false);
        channel_config_set_dreq(&dma_config, pio_get_dreq(host->pio, host->data_sm, true));
        dma_channel_configure(host->dma_channel, &dma_config,
                              &host->pio->txf[host->data_sm], image,
                              SDIO_DATA_WORD_COUNT, false);
        dma_channel_start(host->dma_channel);
        start_tx_data(host);

        start = time_us_32();
        uint32_t profile_start = pf_sd_card_profile_begin();
        if ((block_index + 1u < block_count) &&
            !pf_sdio_pio_encode_data_block(
                blocks + (block_index + 1u) * PF_SD_CARD_BLOCK_SIZE,
                (uint8_t *)images[(block_index + 1u) & 1u])) {
            transfer_ok = false;
        }
        while (!pio_interrupt_get(host->pio, 2u) ||
               dma_channel_is_busy(host->dma_channel)) {
            if ((uint32_t)(time_us_32() - start) > 1000000u) {
                transfer_ok = false;
                pf_sd_card_profile_event(PF_SD_CARD_PROFILE_TIMEOUT);
                break;
            }
            wait_data_idle(host, 2u);
        }
        pf_sd_card_profile_end(PF_SD_CARD_PROFILE_DATA, profile_start);
        pio_sm_set_enabled(host->pio, host->data_sm, false);
        if (dma_channel_is_busy(host->dma_channel)) {
            dma_channel_abort(host->dma_channel);
        }
        if (!transfer_ok || (pio_sm_get_rx_fifo_level(host->pio, host->data_sm) == 0u)) {
            transfer_ok = false;
            break;
        }
        response_word = pio_sm_get(host->pio, host->data_sm);
        if (!pf_sdio_pio_write_response_accepted(response_word)) {
            transfer_ok = false;
            break;
        }
    }

    if (transfer_ok) {
        host->write_stream_next_sector = first_sector + block_count;
        return true;
    }
    (void)pf_sdio_pio_host_recover(host);
    return false;
}