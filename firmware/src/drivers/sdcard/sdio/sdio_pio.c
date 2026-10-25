/** @file Native command/data wire transport; device framing and stream state belong to callers. */
#include "sdio_pio.h"
#include <string.h>

#define TX_FIFO_TIMEOUT_US 100000u
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/regs/pio.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "sdio_pio.pio.h"

#define RESPONSE_IRQ 0u
#define RX_IRQ 1u
#define TX_IRQ 2u

#define DEFAULT_SPEED_MAX_HZ 25000000u
#define HIGH_SPEED_MAX_HZ 50000000u
/* Divider limits in 1/256 system cycles. The clock program spends two cycles per bus period, so 2.0 is the floor. */
#define DIVIDER_MIN_TICKS 512u
#define DIVIDER_MAX_TICKS 0xffffffu
/* The shortened High-Speed RX loop rearms after three cycles; a longer CLK low phase samples one nibble twice. */
#define HIGH_SPEED_DIVIDER_MAX_TICKS 768u

/**
 * @brief Command/clock divider for a requested bus clock, or zero when it cannot be timed safely.
 *
 * Default Speed uses whole-cycle phases, which keep the duty cycle symmetric and never exceed the request.
 * High Speed halves before rounding so the cycle-locked TX data state machine, which runs at exactly half this
 * divider, stays in lockstep with CLK; an odd divider would drift by one cycle every 128 bus periods.
 * Requests faster than the cycle structure allows clamp to the 2.0 divider (system clock / 4).
 */
static uint32_t clock_divider_ticks(uint32_t system_hz, uint32_t clock_hz)
{
    if ((system_hz == 0u) || (clock_hz == 0u) || (clock_hz > HIGH_SPEED_MAX_HZ)) {
        return 0u;
    }
    bool high_speed = clock_hz > DEFAULT_SPEED_MAX_HZ;
    uint64_t ticks = high_speed
        ? 2u * (((uint64_t)system_hz * 64u + clock_hz - 1u) / clock_hz)
        : (((uint64_t)system_hz + 2u * clock_hz - 1u) / (2u * clock_hz)) * 256u;
    if (ticks < DIVIDER_MIN_TICKS) {
        ticks = DIVIDER_MIN_TICKS;
    }
    if ((ticks > DIVIDER_MAX_TICKS) || (high_speed && (ticks > HIGH_SPEED_DIVIDER_MAX_TICKS))) {
        return 0u;
    }
    return (uint32_t)ticks;
}

/** @brief Actual bus clock produced by a divider; rounding down never exceeds the planned request. */
static uint32_t divider_clock_hz(uint32_t system_hz, uint32_t divider_ticks)
{
    return (uint32_t)(((uint64_t)system_hz * 128u) / divider_ticks);
}

/** @brief Fastest clock the planner will time: the High-Speed ceiling, else the Default-Speed ceiling. */
static uint32_t maximum_clock_hz(uint32_t system_hz)
{
    uint32_t ticks = clock_divider_ticks(system_hz, HIGH_SPEED_MAX_HZ);
    return ticks != 0u ? divider_clock_hz(system_hz, ticks) : DEFAULT_SPEED_MAX_HZ;
}

/** @brief Report a physical failure without importing device diagnostics. */
static void wire_error(pf_sdio_pio_t *host, pf_sd_link_sdio_error_t error,
                       pf_sd_link_sdio_phase_t phase)
{
    if (host != NULL) {
        host->last_error = error;
        if (host->error_hook != NULL) {
            host->error_hook(host->hook_context, error, phase);
        }
    }
}

/** @brief Poll using a caller-owned bounded idle callback, or the SDK's bare-metal fallback. */
static void wire_idle(pf_sdio_pio_t *host)
{
    if (host->idle_hook != NULL) {
        host->idle_hook();
    } else {
        tight_loop_contents();
    }
}

/** @brief Yield without consuming a PIO completion flag. */
static void wire_wait(pf_sdio_pio_t *host, uint32_t irq)
{
    if (host->wait_hook != NULL) {
        host->wait_hook(host->pio, irq);
    } else {
        wire_idle(host);
    }
}

/** @brief Clear only the sticky FIFO status owned by the selected state machine. */
static void clear_sm_fdebug(pf_sdio_pio_t *host, uint sm)
{
    uint32_t mask = (1u << (PIO_FDEBUG_TXSTALL_LSB + sm)) |
                    (1u << (PIO_FDEBUG_TXOVER_LSB + sm)) |
                    (1u << (PIO_FDEBUG_RXUNDER_LSB + sm)) |
                    (1u << (PIO_FDEBUG_RXSTALL_LSB + sm));
#if defined(PF_TEST_SDIO_FAKE_PIO)
    host->pio->fdebug &= ~mask;
#else
    host->pio->fdebug = mask;
#endif
}

/** @brief Consume a sticky data FIFO failure and report its direction. */
static bool fifo_stalled(pf_sdio_pio_t *host, bool tx)
{
    uint32_t mask = 1u << ((tx ? PIO_FDEBUG_TXSTALL_LSB : PIO_FDEBUG_RXSTALL_LSB) + host->data_sm);
    if ((host->pio->fdebug & mask) == 0u) {
        return false;
    }
#if defined(PF_TEST_SDIO_FAKE_PIO)
    host->pio->fdebug &= ~mask;
#else
    host->pio->fdebug = mask;
#endif
    wire_error(host, tx ? PF_SD_LINK_SDIO_ERROR_TX_STALL : PF_SD_LINK_SDIO_ERROR_RX_STALL,
               PF_SD_LINK_SDIO_PHASE_DATA);
    return true;
}

/** @brief Distinguish DMA starvation, PIO noncompletion, and a held-low data line. */
static void data_timeout(pf_sdio_pio_t *host, bool tx, bool dma_busy)
{
    pf_sd_link_sdio_error_t error = PF_SD_LINK_SDIO_ERROR_PIO_TIMEOUT;
    if (dma_busy) {
        error = PF_SD_LINK_SDIO_ERROR_DMA_TIMEOUT;
    } else if (tx && pio_sm_get_rx_fifo_level(host->pio, host->data_sm) != 0u) {
        error = PF_SD_LINK_SDIO_ERROR_BUSY_TIMEOUT;
    }
    wire_error(host, error, PF_SD_LINK_SDIO_PHASE_DATA);
}

static const pio_program_t *data_rx_program(const pf_sdio_pio_t *host)
{
    return host->data_rx_fast ? &pf_sdio_pio_data_rx_fast_program : &pf_sdio_pio_data_rx_program;
}

static const pio_program_t *data_tx_program(const pf_sdio_pio_t *host)
{
    return host->data_tx_fast ? &pf_sdio_pio_data_tx_fast_program : &pf_sdio_pio_data_tx_program;
}

/** @brief Claim instruction memory for bidirectional commands and command-owned CLK. */
static bool load_command_program(pf_sdio_pio_t *host)
{
    if (!pio_can_add_program(host->pio, &pf_sdio_pio_command_program)) {
        return false;
    }
    host->command_program_offset = pio_add_program(host->pio, &pf_sdio_pio_command_program);
    host->command_program_loaded = true;
    pio_sm_config config = pf_sdio_pio_command_program_get_default_config(host->command_program_offset);
    sm_config_set_out_pins(&config, host->config.pins.command_gpio, 1u);
    sm_config_set_set_pins(&config, host->config.pins.command_gpio, 1u);
    sm_config_set_in_pins(&config, host->config.pins.command_gpio);
    sm_config_set_jmp_pin(&config, host->config.pins.command_gpio);
    sm_config_set_sideset_pins(&config, host->config.pins.clock_gpio);
    sm_config_set_out_shift(&config, false, true, 32u);
    sm_config_set_in_shift(&config, false, true, 32u);
    sm_config_set_mov_status(&config, STATUS_TX_LESSTHAN, 2u);
    pio_sm_init(host->pio, host->command_sm, host->command_program_offset, &config);
    pio_sm_set_consecutive_pindirs(host->pio, host->command_sm, host->config.pins.clock_gpio, 1u, true);
    return true;
}

/** @brief Claim and configure the RX program selected by the last clock request's card timing mode. */
static bool load_data_rx_program(pf_sdio_pio_t *host)
{
    if (!pio_can_add_program(host->pio, data_rx_program(host))) {
        return false;
    }
    host->data_rx_program_offset = pio_add_program(host->pio, data_rx_program(host));
    pio_sm_config config = host->data_rx_fast
        ? pf_sdio_pio_data_rx_fast_program_get_default_config(host->data_rx_program_offset)
        : pf_sdio_pio_data_rx_program_get_default_config(host->data_rx_program_offset);
    sm_config_set_in_pins(&config, host->config.pins.data_gpio[0]);
    sm_config_set_jmp_pin(&config, host->config.pins.data_gpio[0]);
    sm_config_set_in_shift(&config, false, true, 32u);
    sm_config_set_out_shift(&config, false, false, 32u);
    pio_sm_init(host->pio, host->data_sm, host->data_rx_program_offset, &config);
    pio_sm_set_consecutive_pindirs(host->pio, host->data_sm, host->config.pins.data_gpio[0], 4u, false);
    host->data_rx_program_loaded = true;
    return true;
}

bool pf_sdio_pio_tx_prepare(pf_sdio_pio_t *host)
{
    if ((host == NULL) || (host->pio == NULL) ||
        ((host->clock_owner != PF_SDIO_PIO_CLOCK_COMMAND) &&
         (host->clock_owner != PF_SDIO_PIO_CLOCK_UNOWNED))) {
        wire_error(host, PF_SD_LINK_SDIO_ERROR_STATE, PF_SD_LINK_SDIO_PHASE_SETUP);
        return false;
    }
    pio_sm_set_enabled(host->pio, host->command_sm, false);
    pio_sm_set_enabled(host->pio, host->data_sm, false);
    if (host->data_rx_program_loaded) {
        pio_remove_program(host->pio, data_rx_program(host), host->data_rx_program_offset);
        host->data_rx_program_loaded = false;
    }
    if (host->command_program_loaded) {
        pio_remove_program(host->pio, &pf_sdio_pio_command_program, host->command_program_offset);
        host->command_program_loaded = false;
    }
    if (!pio_can_add_program(host->pio, &pf_sdio_pio_clock_program) ||
        !pio_can_add_program(host->pio, data_tx_program(host))) {
        wire_error(host, PF_SD_LINK_SDIO_ERROR_STATE, PF_SD_LINK_SDIO_PHASE_SETUP);
        return false;
    }
    host->clock_program_offset = pio_add_program(host->pio, &pf_sdio_pio_clock_program);
    host->clock_program_loaded = true;
    host->data_tx_program_offset = pio_add_program(host->pio, data_tx_program(host));
    host->data_tx_program_loaded = true;
    pio_sm_config config = pf_sdio_pio_clock_program_get_default_config(host->clock_program_offset);
    sm_config_set_sideset_pins(&config, host->config.pins.clock_gpio);
    pio_sm_init(host->pio, host->command_sm, host->clock_program_offset, &config);
    pio_sm_set_consecutive_pindirs(host->pio, host->command_sm, host->config.pins.clock_gpio, 1u, true);
    pio_sm_set_clkdiv_int_frac(host->pio, host->command_sm, (uint16_t)(host->clock_div_ticks >> 8u),
                              (uint8_t)(host->clock_div_ticks & 0xffu));
    config = host->data_tx_fast
        ? pf_sdio_pio_data_tx_fast_program_get_default_config(host->data_tx_program_offset)
        : pf_sdio_pio_data_tx_program_get_default_config(host->data_tx_program_offset);
    if (host->data_tx_fast) {
        sm_config_set_clkdiv_int_frac(&config, (uint16_t)(host->clock_div_ticks >> 9u),
                                      (uint8_t)((host->clock_div_ticks >> 1u) & 0xffu));
    }
    sm_config_set_out_pins(&config, host->config.pins.data_gpio[0], 4u);
    sm_config_set_set_pins(&config, host->config.pins.data_gpio[0], 4u);
    sm_config_set_in_pins(&config, host->config.pins.data_gpio[0]);
    sm_config_set_out_shift(&config, false, true, 32u);
    sm_config_set_in_shift(&config, false, false, 32u);
    pio_sm_init(host->pio, host->data_sm, host->data_tx_program_offset, &config);
    pio_sm_clear_fifos(host->pio, host->command_sm);
    pio_sm_clear_fifos(host->pio, host->data_sm);
    clear_sm_fdebug(host, host->command_sm);
    clear_sm_fdebug(host, host->data_sm);
    pio_clkdiv_restart_sm_mask(host->pio, (1u << host->command_sm) | (1u << host->data_sm));
    host->clock_owner = PF_SDIO_PIO_CLOCK_DATA_TX;
    return true;
}

bool pf_sdio_pio_restore_command(pf_sdio_pio_t *host)
{
    if ((host == NULL) || (host->pio == NULL)) {
        return false;
    }
    host->mode_restore_failed = true;
    pio_sm_set_enabled(host->pio, host->command_sm, false);
    pio_sm_set_enabled(host->pio, host->data_sm, false);
    if (host->dma_claimed && dma_channel_is_busy(host->dma_channel)) {
        dma_channel_abort(host->dma_channel);
    }
    if (host->data_tx_program_loaded) {
        pio_remove_program(host->pio, data_tx_program(host), host->data_tx_program_offset);
        host->data_tx_program_loaded = false;
    }
    if (host->clock_program_loaded) {
        pio_remove_program(host->pio, &pf_sdio_pio_clock_program, host->clock_program_offset);
        host->clock_program_loaded = false;
    }
    if ((!host->command_program_loaded && !load_command_program(host)) ||
        (!host->data_rx_program_loaded && !load_data_rx_program(host))) {
        wire_error(host, PF_SD_LINK_SDIO_ERROR_STATE, PF_SD_LINK_SDIO_PHASE_SETUP);
        return false;
    }
    pio_sm_clear_fifos(host->pio, host->command_sm);
    pio_sm_clear_fifos(host->pio, host->data_sm);
    for (uint irq = 0u; irq < 3u; irq++) {
        pio_interrupt_clear(host->pio, irq);
    }
    pio_sm_restart(host->pio, host->command_sm);
    pio_sm_restart(host->pio, host->data_sm);
    pio_sm_exec(host->pio, host->command_sm, pio_encode_jmp(host->command_program_offset));
    pio_sm_exec(host->pio, host->data_sm, pio_encode_jmp(host->data_rx_program_offset));
    pio_sm_set_clkdiv_int_frac(host->pio, host->command_sm, (uint16_t)(host->clock_div_ticks >> 8u),
                              (uint8_t)(host->clock_div_ticks & 0xffu));
    pio_clkdiv_restart_sm_mask(host->pio, (1u << host->command_sm) | (1u << host->data_sm));
    pio_sm_set_enabled(host->pio, host->command_sm, true);
    host->clock_owner = PF_SDIO_PIO_CLOCK_COMMAND;
    host->mode_restore_failed = false;
    return true;
}

/** @brief Release CMD/DAT to pulled-up inputs while retaining CLK output ownership. */
static void release_bus_pins(pf_sdio_pio_t *host)
{
    gpio_pull_up(host->config.pins.command_gpio);
    for (uint pin = 0u; pin < 4u; pin++) {
        gpio_pull_up(host->config.pins.data_gpio[0] + pin);
    }
    pio_sm_set_consecutive_pindirs(host->pio, host->command_sm, host->config.pins.command_gpio, 1u, false);
    pio_sm_set_consecutive_pindirs(host->pio, host->data_sm, host->config.pins.data_gpio[0], 4u, false);
    pio_sm_set_consecutive_pindirs(host->pio, host->command_sm, host->config.pins.clock_gpio, 1u, true);
    pio_sm_exec(host->pio, host->command_sm, pio_encode_nop());
}

void pf_sdio_pio_abort(pf_sdio_pio_t *host)
{
    if ((host == NULL) || (host->pio == NULL)) {
        return;
    }
    pio_sm_set_enabled(host->pio, host->command_sm, false);
    pio_sm_set_enabled(host->pio, host->data_sm, false);
    if (host->dma_claimed) {
        dma_channel_abort(host->dma_channel);
    }
    clear_sm_fdebug(host, host->command_sm);
    clear_sm_fdebug(host, host->data_sm);
    pio_sm_clear_fifos(host->pio, host->command_sm);
    pio_sm_clear_fifos(host->pio, host->data_sm);
    for (uint irq = 0u; irq < 3u; irq++) {
        pio_interrupt_clear(host->pio, irq);
    }
    pio_sm_restart(host->pio, host->command_sm);
    pio_sm_restart(host->pio, host->data_sm);
    pio_clkdiv_restart_sm_mask(host->pio, (1u << host->command_sm) | (1u << host->data_sm));
    release_bus_pins(host);
    if (!pf_sdio_pio_restore_command(host)) {
        release_bus_pins(host);
        host->clock_owner = PF_SDIO_PIO_CLOCK_UNOWNED;
        return;
    }
    pio_sm_set_enabled(host->pio, host->command_sm, false);
    pio_sm_set_enabled(host->pio, host->data_sm, false);
    clear_sm_fdebug(host, host->command_sm);
    clear_sm_fdebug(host, host->data_sm);
    pio_sm_clear_fifos(host->pio, host->command_sm);
    pio_sm_clear_fifos(host->pio, host->data_sm);
    pio_sm_restart(host->pio, host->command_sm);
    pio_sm_restart(host->pio, host->data_sm);
    pio_clkdiv_restart_sm_mask(host->pio, (1u << host->command_sm) | (1u << host->data_sm));
    release_bus_pins(host);
    pio_sm_exec(host->pio, host->command_sm, pio_encode_jmp(host->command_program_offset));
    host->clock_owner = PF_SDIO_PIO_CLOCK_UNOWNED;
}

bool pf_sdio_pio_clocks(pf_sdio_pio_t *host, uint32_t cycles, uint32_t timeout_us)
{
    if ((host == NULL) || (host->pio == NULL) || cycles == 0u || timeout_us == 0u ||
        !host->dma_claimed || host->mode_restore_failed ||
        ((host->clock_owner != PF_SDIO_PIO_CLOCK_COMMAND) &&
         (host->clock_owner != PF_SDIO_PIO_CLOCK_UNOWNED))) {
        wire_error(host, PF_SD_LINK_SDIO_ERROR_STATE, PF_SD_LINK_SDIO_PHASE_SETUP);
        return false;
    }
    pf_sdio_pio_abort(host);
    if (host->mode_restore_failed || !pf_sdio_pio_tx_prepare(host)) {
        pf_sdio_pio_abort(host);
        return false;
    }
    pio_sm_set_consecutive_pindirs(host->pio, host->command_sm, host->config.pins.command_gpio, 1u, false);
    pio_sm_set_consecutive_pindirs(host->pio, host->data_sm, host->config.pins.data_gpio[0], 4u, false);
    pio_sm_set_consecutive_pindirs(host->pio, host->command_sm, host->config.pins.clock_gpio, 1u, true);
    pio_sm_exec(host->pio, host->command_sm, pio_encode_jmp(host->clock_program_offset));
    host->clock_owner = PF_SDIO_PIO_CLOCK_INITIALIZATION;
    pio_sm_set_enabled(host->pio, host->command_sm, true);
    uint32_t start = time_us_32();
    for (uint32_t cycle = 0u; cycle < cycles; cycle++) {
        if ((uint32_t)(time_us_32() - start) >= timeout_us) {
            wire_error(host, PF_SD_LINK_SDIO_ERROR_PIO_TIMEOUT, PF_SD_LINK_SDIO_PHASE_SETUP);
            pf_sdio_pio_abort(host);
            return false;
        }
        for (uint level = 0u; level < 2u; level++) {
            while ((gpio_get(host->config.pins.clock_gpio) != 0u) != (level == 0u)) {
                if ((uint32_t)(time_us_32() - start) >= timeout_us) {
                    wire_error(host, PF_SD_LINK_SDIO_ERROR_PIO_TIMEOUT, PF_SD_LINK_SDIO_PHASE_SETUP);
                    pf_sdio_pio_abort(host);
                    return false;
                }
                tight_loop_contents();
            }
        }
    }
    pio_sm_set_enabled(host->pio, host->command_sm, false);
    if (!pf_sdio_pio_restore_command(host)) {
        pf_sdio_pio_abort(host);
        return false;
    }
    pio_sm_set_enabled(host->pio, host->command_sm, false);
    release_bus_pins(host);
    pio_sm_exec(host->pio, host->command_sm, pio_encode_jmp(host->command_program_offset));
    host->clock_owner = PF_SDIO_PIO_CLOCK_UNOWNED;
    return true;
}

bool pf_sdio_pio_set_clock(pf_sdio_pio_t *host, uint32_t clock_hz)
{
    uint32_t divider_ticks = host != NULL ? clock_divider_ticks(host->system_clock_hz, clock_hz) : 0u;
    if ((host == NULL) || (host->pio == NULL) || (divider_ticks == 0u) || host->mode_restore_failed ||
        ((host->clock_owner != PF_SDIO_PIO_CLOCK_UNOWNED) &&
         (host->clock_owner != PF_SDIO_PIO_CLOCK_COMMAND))) {
        wire_error(host, PF_SD_LINK_SDIO_ERROR_STATE, PF_SD_LINK_SDIO_PHASE_SETUP);
        return false;
    }
    /* Card output timing follows the requested mode, so a clamped High-Speed request keeps High-Speed sampling. */
    bool high_speed = clock_hz > DEFAULT_SPEED_MAX_HZ;
    bool resume = host->clock_owner == PF_SDIO_PIO_CLOCK_COMMAND;
    pio_sm_set_enabled(host->pio, host->command_sm, false);
    host->clock_div_ticks = divider_ticks;
    pio_sm_set_clkdiv_int_frac(host->pio, host->command_sm, (uint16_t)(divider_ticks >> 8u),
                              (uint8_t)(divider_ticks & 0xffu));
    host->current_clock_hz = divider_clock_hz(host->system_clock_hz, divider_ticks);
    if (host->command_program_loaded) {
        pio_sm_set_enabled(host->pio, host->data_sm, false);
        if (host->data_rx_program_loaded) {
            pio_remove_program(host->pio, data_rx_program(host), host->data_rx_program_offset);
        }
        host->data_rx_program_loaded = false;
        host->data_rx_fast = high_speed;
        host->data_tx_fast = high_speed;
        if (!load_data_rx_program(host)) {
            host->mode_restore_failed = true;
            wire_error(host, PF_SD_LINK_SDIO_ERROR_STATE, PF_SD_LINK_SDIO_PHASE_SETUP);
            return false;
        }
    } else {
        host->data_rx_fast = high_speed;
        host->data_tx_fast = high_speed;
    }
    pio_clkdiv_restart_sm_mask(host->pio, (1u << host->command_sm) | (1u << host->data_sm));
    pio_sm_set_enabled(host->pio, host->command_sm, resume);
    return true;
}

bool pf_sdio_pio_init(pf_sdio_pio_t *host, const pf_sdio_pio_config_t *config)
{
    uint32_t pin_limit = NUM_BANK0_GPIOS < 32u ? NUM_BANK0_GPIOS : 32u;
    if ((host == NULL) || (host->pio != NULL) || (config == NULL) ||
        ((config->pio != pio0) && (config->pio != pio1)) ||
        (config->pins.command_gpio >= pin_limit) || (config->pins.clock_gpio >= pin_limit) ||
        (config->pins.data_gpio[0] > pin_limit - 4u) ||
        (config->pins.clock_gpio != ((config->pins.data_gpio[0] + 30u) % 32u)) ||
        !pio_can_add_program(config->pio, &pf_sdio_pio_command_program)) {
        return false;
    }
    for (uint pin = 0u; pin < 4u; pin++) {
        if ((config->pins.data_gpio[pin] != config->pins.data_gpio[0] + pin) ||
            (config->pins.command_gpio == config->pins.data_gpio[pin]) ||
            (config->pins.clock_gpio == config->pins.data_gpio[pin])) {
            return false;
        }
    }
    if (config->pins.command_gpio == config->pins.clock_gpio) {
        return false;
    }
    pf_sdio_pio_config_t saved = *config;
    memset(host, 0, sizeof(*host));
    host->config = saved;
    host->pio = saved.pio;
    host->system_clock_hz = clock_get_hz(clk_sys);
    int sm = pio_claim_unused_sm(host->pio, false);
    if (sm < 0) {
        memset(host, 0, sizeof(*host));
        return false;
    }
    host->command_sm = (uint)sm;
    sm = pio_claim_unused_sm(host->pio, false);
    if (sm < 0) {
        pio_sm_unclaim(host->pio, host->command_sm);
        memset(host, 0, sizeof(*host));
        return false;
    }
    host->data_sm = (uint)sm;
    int dma = dma_claim_unused_channel(false);
    if (dma < 0) {
        pio_sm_unclaim(host->pio, host->data_sm);
        pio_sm_unclaim(host->pio, host->command_sm);
        memset(host, 0, sizeof(*host));
        return false;
    }
    host->dma_channel = (uint)dma;
    host->dma_claimed = true;
    pio_gpio_init(host->pio, host->config.pins.clock_gpio);
    pio_gpio_init(host->pio, host->config.pins.command_gpio);
    host->input_sync_bypass_mask = (1u << host->config.pins.clock_gpio) | (1u << host->config.pins.command_gpio);
    for (uint pin = 0u; pin < 4u; pin++) {
        pio_gpio_init(host->pio, host->config.pins.data_gpio[0] + pin);
        host->input_sync_bypass_mask |= 1u << (host->config.pins.data_gpio[0] + pin);
    }
    host->previous_input_sync_bypass = host->pio->input_sync_bypass;
    host->pio->input_sync_bypass |= host->input_sync_bypass_mask;
    if (!load_command_program(host) || !load_data_rx_program(host) ||
        !pf_sdio_pio_set_clock(host, 400000u)) {
        pf_sdio_pio_deinit(host);
        return false;
    }
    release_bus_pins(host);
    pio_sm_exec(host->pio, host->command_sm, pio_encode_jmp(host->command_program_offset));
    pio_sm_set_enabled(host->pio, host->command_sm, false);
    host->clock_owner = PF_SDIO_PIO_CLOCK_UNOWNED;
    return true;
}

void pf_sdio_pio_deinit(pf_sdio_pio_t *host)
{
    if ((host == NULL) || (host->pio == NULL)) {
        return;
    }
    pio_sm_set_enabled(host->pio, host->command_sm, false);
    pio_sm_set_enabled(host->pio, host->data_sm, false);
    if (host->dma_claimed) {
        dma_channel_abort(host->dma_channel);
    }
    clear_sm_fdebug(host, host->command_sm);
    clear_sm_fdebug(host, host->data_sm);
    pio_sm_clear_fifos(host->pio, host->command_sm);
    pio_sm_clear_fifos(host->pio, host->data_sm);
    for (uint irq = 0u; irq < 3u; irq++) {
        pio_interrupt_clear(host->pio, irq);
    }
    pio_sm_restart(host->pio, host->command_sm);
    pio_sm_restart(host->pio, host->data_sm);
    pio_clkdiv_restart_sm_mask(host->pio, (1u << host->command_sm) | (1u << host->data_sm));
    release_bus_pins(host);
    if (host->command_program_loaded) {
        pio_sm_exec(host->pio, host->command_sm, pio_encode_jmp(host->command_program_offset));
    }
    if (host->data_rx_program_loaded) {
        pio_remove_program(host->pio, data_rx_program(host), host->data_rx_program_offset);
    }
    if (host->command_program_loaded) {
        pio_remove_program(host->pio, &pf_sdio_pio_command_program, host->command_program_offset);
    }
    if (host->data_tx_program_loaded) {
        pio_remove_program(host->pio, data_tx_program(host), host->data_tx_program_offset);
    }
    if (host->clock_program_loaded) {
        pio_remove_program(host->pio, &pf_sdio_pio_clock_program, host->clock_program_offset);
    }
    host->pio->input_sync_bypass = (host->pio->input_sync_bypass & ~host->input_sync_bypass_mask) |
                                  (host->previous_input_sync_bypass & host->input_sync_bypass_mask);
    gpio_init(host->config.pins.command_gpio);
    gpio_pull_up(host->config.pins.command_gpio);
    gpio_set_dir(host->config.pins.command_gpio, false);
    for (uint pin = 0u; pin < 4u; pin++) {
        gpio_init(host->config.pins.data_gpio[0] + pin);
        gpio_pull_up(host->config.pins.data_gpio[0] + pin);
        gpio_set_dir(host->config.pins.data_gpio[0] + pin, false);
    }
    gpio_init(host->config.pins.clock_gpio);
    gpio_put(host->config.pins.clock_gpio, false);
    gpio_set_dir(host->config.pins.clock_gpio, true);
    if (host->dma_claimed) {
        dma_channel_unclaim(host->dma_channel);
    }
    pio_sm_unclaim(host->pio, host->data_sm);
    pio_sm_unclaim(host->pio, host->command_sm);
    memset(host, 0, sizeof(*host));
}

bool pf_sdio_pio_exchange(pf_sdio_pio_t *host, const uint8_t *tx, size_t tx_size,
    uint16_t rx_bits, uint8_t *rx, size_t capacity, size_t *received, uint32_t timeout_us)
{
    if (received != NULL) {
        *received = 0u;
    }
    size_t count = (rx_bits + 31u) / 32u;
    if ((host == NULL) || (host->pio == NULL) || tx == NULL || tx_size != 6u ||
        received == NULL || timeout_us == 0u || host->mode_restore_failed ||
        !host->command_program_loaded || (host->current_clock_hz == 0u) ||
        ((host->clock_owner != PF_SDIO_PIO_CLOCK_UNOWNED) &&
         (host->clock_owner != PF_SDIO_PIO_CLOCK_COMMAND)) ||
        (rx_bits != 0u && (rx == NULL || capacity < rx_bits / 8u)) ||
        rx_bits > 136u || rx_bits % 8u != 0u) {
        wire_error(host, PF_SD_LINK_SDIO_ERROR_STATE, PF_SD_LINK_SDIO_PHASE_COMMAND);
        return false;
    }
    if (host->clock_owner == PF_SDIO_PIO_CLOCK_UNOWNED) {
        host->clock_owner = PF_SDIO_PIO_CLOCK_COMMAND;
        pio_sm_set_enabled(host->pio, host->command_sm, true);
    }
    uint64_t bits = ((uint64_t)tx[0] << 40u) | ((uint64_t)tx[1] << 32u) |
                    ((uint64_t)tx[2] << 24u) | ((uint64_t)tx[3] << 16u) |
                    ((uint64_t)tx[4] << 8u) | tx[5];
    pio_sm_clear_fifos(host->pio, host->command_sm);
    pio_interrupt_clear(host->pio, RESPONSE_IRQ);
    pio_sm_put_blocking(host->pio, host->command_sm, (47u << 26u) | (uint32_t)(bits >> 22u));
    pio_sm_put_blocking(host->pio, host->command_sm, (uint32_t)(bits & 0x3fffffu) << 10u);
    pio_sm_put_blocking(host->pio, host->command_sm,
                        rx_bits == 0u ? 0u : ((uint32_t)(rx_bits - 2u) << 24u));
    uint32_t start = time_us_32();
    size_t read = 0u;
    while (!pio_interrupt_get(host->pio, RESPONSE_IRQ) || (read < count)) {
        while ((read < count) && (pio_sm_get_rx_fifo_level(host->pio, host->command_sm) > 0u)) {
            uint32_t word = pio_sm_get(host->pio, host->command_sm);
            size_t bytes = rx_bits / 8u - *received;
            if (bytes > 4u) {
                bytes = 4u;
            }
            for (size_t index = 0u; index < bytes; ++index) {
                rx[*received + index] = (uint8_t)(word >> ((bytes - index - 1u) * 8u));
            }
            *received += bytes;
            ++read;
        }
        if (((uint32_t)(time_us_32() - start) >= timeout_us) &&
            (!pio_interrupt_get(host->pio, RESPONSE_IRQ) || (read < count))) {
            wire_error(host, PF_SD_LINK_SDIO_ERROR_PIO_TIMEOUT, PF_SD_LINK_SDIO_PHASE_COMMAND);
            pio_sm_clear_fifos(host->pio, host->command_sm);
            pio_sm_restart(host->pio, host->command_sm);
            pio_sm_exec(host->pio, host->command_sm, pio_encode_jmp(host->command_program_offset));
            return false;
        }
        tight_loop_contents();
    }
    pio_interrupt_clear(host->pio, RESPONSE_IRQ);
    sleep_us((8000000u + host->current_clock_hz - 1u) / host->current_clock_hz);
    return true;
}

bool pf_sdio_pio_rx_start(pf_sdio_pio_t *host, uint32_t *destination,
    uint32_t words, uint32_t nibbles, uint32_t blocks)
{
    if ((host == NULL) || (host->pio == NULL) || !host->dma_claimed ||
        !host->data_rx_program_loaded || host->mode_restore_failed ||
        ((host->clock_owner != PF_SDIO_PIO_CLOCK_UNOWNED) &&
         (host->clock_owner != PF_SDIO_PIO_CLOCK_COMMAND)) ||
        (destination == NULL) || (((uintptr_t)destination & 3u) != 0u) ||
        (words == 0u) || (nibbles == 0u) || ((nibbles & 7u) != 0u) || (blocks == 0u) ||
        ((uint64_t)(nibbles / 8u) * blocks != words)) {
        wire_error(host, PF_SD_LINK_SDIO_ERROR_STATE, PF_SD_LINK_SDIO_PHASE_DATA);
        return false;
    }
    pio_sm_set_enabled(host->pio, host->data_sm, false);
    clear_sm_fdebug(host, host->data_sm);
    pio_sm_clear_fifos(host->pio, host->data_sm);
    pio_interrupt_clear(host->pio, RX_IRQ);
    pio_sm_restart(host->pio, host->data_sm);
    pio_sm_exec(host->pio, host->data_sm, pio_encode_jmp(host->data_rx_program_offset));
    if (blocks > 1u) {
        pio_sm_put_blocking(host->pio, host->data_sm, blocks - 1u);
        pio_sm_exec(host->pio, host->data_sm, pio_encode_pull(false, true));
        pio_sm_exec(host->pio, host->data_sm, pio_encode_mov(pio_y, pio_osr));
    } else {
        pio_sm_exec(host->pio, host->data_sm, pio_encode_set(pio_y, 0u));
    }
    pio_sm_put_blocking(host->pio, host->data_sm, nibbles - 1u);
    dma_channel_config config = dma_channel_get_default_config(host->dma_channel);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
    channel_config_set_bswap(&config, true);
    channel_config_set_read_increment(&config, false);
    channel_config_set_write_increment(&config, true);
    channel_config_set_dreq(&config, pio_get_dreq(host->pio, host->data_sm, false));
    dma_channel_configure(host->dma_channel, &config, destination, &host->pio->rxf[host->data_sm], words, false);
    dma_channel_start(host->dma_channel);
    pio_sm_set_enabled(host->pio, host->data_sm, true);
    return true;
}

bool pf_sdio_pio_rx_poll(pf_sdio_pio_t *host, uint32_t start,
    uint32_t timeout_us, bool *complete)
{
    if ((host == NULL) || (host->pio == NULL) || !host->dma_claimed || (complete == NULL)) {
        wire_error(host, PF_SD_LINK_SDIO_ERROR_STATE, PF_SD_LINK_SDIO_PHASE_DATA);
        return false;
    }
    *complete = pio_interrupt_get(host->pio, RX_IRQ) && !dma_channel_is_busy(host->dma_channel);
    if (*complete) {
        return true;
    }
    if (fifo_stalled(host, false)) {
        return false;
    }
    if ((uint32_t)(time_us_32() - start) > timeout_us) {
        data_timeout(host, false, dma_channel_is_busy(host->dma_channel));
        return false;
    }
    return true;
}

void pf_sdio_pio_rx_wait(pf_sdio_pio_t *host)
{
    if ((host != NULL) && (host->pio != NULL)) {
        wire_wait(host, RX_IRQ);
    }
}

bool pf_sdio_pio_rx_finish(pf_sdio_pio_t *host, uint32_t timeout_us)
{
    uint32_t start = time_us_32();
    bool complete;
    for (;;) {
        if (!pf_sdio_pio_rx_poll(host, start, timeout_us, &complete)) {
            return false;
        }
        if (complete) {
            dma_channel_wait_for_finish_blocking(host->dma_channel);
            return true;
        }
        wire_wait(host, RX_IRQ);
    }
}

void pf_sdio_pio_rx_cleanup(pf_sdio_pio_t *host)
{
    if ((host == NULL) || (host->pio == NULL)) {
        return;
    }
    pio_sm_set_enabled(host->pio, host->data_sm, false);
    if (host->dma_claimed && dma_channel_is_busy(host->dma_channel)) {
        dma_channel_abort(host->dma_channel);
    }
    clear_sm_fdebug(host, host->data_sm);
    pio_sm_clear_fifos(host->pio, host->data_sm);
    pio_interrupt_clear(host->pio, RX_IRQ);
    pio_sm_restart(host->pio, host->data_sm);
    pio_sm_exec(host->pio, host->data_sm, pio_encode_jmp(host->data_rx_program_offset));
}

bool pf_sdio_pio_rx_progress(pf_sdio_pio_t *host, const uint32_t *buffer,
    uint32_t capacity_words, uint32_t *completed_words)
{
    if ((host == NULL) || !host->dma_claimed || (buffer == NULL) || (completed_words == NULL) ||
        (capacity_words > UINT32_MAX / sizeof(uint32_t))) {
        wire_error(host, PF_SD_LINK_SDIO_ERROR_STATE, PF_SD_LINK_SDIO_PHASE_DATA);
        return false;
    }
    uint32_t write_addr = dma_channel_hw_addr(host->dma_channel)->write_addr;
    uint32_t start = (uint32_t)(uintptr_t)buffer;
    if ((write_addr < start) || (write_addr - start > capacity_words * sizeof(uint32_t)) ||
        ((write_addr & (sizeof(uint32_t) - 1u)) != 0u)) {
        wire_error(host, PF_SD_LINK_SDIO_ERROR_STATE, PF_SD_LINK_SDIO_PHASE_DATA);
        return false;
    }
    *completed_words = (write_addr - start) / sizeof(uint32_t);
    __dmb();
    return true;
}

bool pf_sdio_pio_tx_start(pf_sdio_pio_t *host, const uint32_t *image,
    uint32_t words, uint32_t nibbles)
{
    if ((host == NULL) || (host->pio == NULL) || !host->dma_claimed ||
        !host->data_tx_program_loaded || host->mode_restore_failed ||
        (host->clock_owner != PF_SDIO_PIO_CLOCK_DATA_TX) ||
        (image == NULL) || (((uintptr_t)image & 3u) != 0u) ||
        (words == 0u) || (nibbles == 0u) || ((uint64_t)words * 8u != nibbles)) {
        wire_error(host, PF_SD_LINK_SDIO_ERROR_STATE, PF_SD_LINK_SDIO_PHASE_DATA);
        return false;
    }
    pio_sm_clear_fifos(host->pio, host->data_sm);
    pio_interrupt_clear(host->pio, TX_IRQ);
    pio_sm_restart(host->pio, host->data_sm);
    pio_sm_exec(host->pio, host->data_sm, pio_encode_jmp(host->data_tx_program_offset));
    pio_sm_put_blocking(host->pio, host->data_sm, nibbles - 1u);
    dma_channel_config config = dma_channel_get_default_config(host->dma_channel);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
    channel_config_set_bswap(&config, true);
    channel_config_set_read_increment(&config, true);
    channel_config_set_write_increment(&config, false);
    channel_config_set_dreq(&config, pio_get_dreq(host->pio, host->data_sm, true));
    dma_channel_configure(host->dma_channel, &config, &host->pio->txf[host->data_sm], image, words, false);
    dma_channel_start(host->dma_channel);
    uint32_t start = time_us_32();
    while (pio_sm_get_tx_fifo_level(host->pio, host->data_sm) == 0u) {
        if ((uint32_t)(time_us_32() - start) >= TX_FIFO_TIMEOUT_US) {
            data_timeout(host, true, true);
            return false;
        }
        tight_loop_contents();
    }
    if (host->data_tx_fast) {
        pio_sm_set_enabled(host->pio, host->command_sm, false);
        pio_sm_restart(host->pio, host->command_sm);
        pio_sm_restart(host->pio, host->data_sm);
        pio_sm_exec(host->pio, host->command_sm, pio_encode_jmp(host->clock_program_offset));
        pio_clkdiv_restart_sm_mask(host->pio, (1u << host->command_sm) | (1u << host->data_sm));
        pio_enable_sm_mask_in_sync(host->pio, (1u << host->command_sm) | (1u << host->data_sm));
    } else {
        pio_sm_set_enabled(host->pio, host->command_sm, true);
        pio_sm_set_enabled(host->pio, host->data_sm, true);
    }
    return true;
}

bool pf_sdio_pio_tx_finish(pf_sdio_pio_t *host, uint32_t timeout_us,
    uint32_t *response_word)
{
    if ((host == NULL) || (host->pio == NULL) || !host->dma_claimed ||
        !host->data_tx_program_loaded || (response_word == NULL)) {
        wire_error(host, PF_SD_LINK_SDIO_ERROR_STATE, PF_SD_LINK_SDIO_PHASE_DATA);
        return false;
    }
    uint32_t start = time_us_32();
    bool success = false;
    for (;;) {
        if (fifo_stalled(host, true)) {
            break;
        }
        bool irq = pio_interrupt_get(host->pio, TX_IRQ);
        bool busy = dma_channel_is_busy(host->dma_channel);
        if (irq && !busy) {
            success = !fifo_stalled(host, true);
            break;
        }
        if ((uint32_t)(time_us_32() - start) > timeout_us) {
            data_timeout(host, true, busy);
            break;
        }
        wire_wait(host, TX_IRQ);
    }
    pio_sm_set_enabled(host->pio, host->data_sm, false);
    if (dma_channel_is_busy(host->dma_channel)) {
        dma_channel_abort(host->dma_channel);
    }
    if (!success) {
        return false;
    }
    if (pio_sm_get_rx_fifo_level(host->pio, host->data_sm) == 0u) {
        wire_error(host, PF_SD_LINK_SDIO_ERROR_STATE, PF_SD_LINK_SDIO_PHASE_DATA);
        return false;
    }
    *response_word = pio_sm_get(host->pio, host->data_sm);
    return true;
}

bool pf_sdio_pio_wait_ready(pf_sdio_pio_t *host, uint32_t timeout_us)
{
    if ((host == NULL) || (host->pio == NULL)) {
        wire_error(host, PF_SD_LINK_SDIO_ERROR_STATE, PF_SD_LINK_SDIO_PHASE_BUSY);
        return false;
    }
    uint32_t start = time_us_32();
    while (gpio_get(host->config.pins.data_gpio[0]) == 0u) {
        if ((uint32_t)(time_us_32() - start) > timeout_us) {
            wire_error(host, PF_SD_LINK_SDIO_ERROR_BUSY_TIMEOUT, PF_SD_LINK_SDIO_PHASE_BUSY);
            return false;
        }
        wire_idle(host);
    }
    return true;
}

pf_sd_link_result_t pf_sd_link_init(const pf_sd_link_t *bus)
{
    if (!pf_sd_link_valid(bus, PF_SD_LINK_NATIVE_SD)) {
        return PF_SD_LINK_INVALID_ARGUMENT;
    }
    pf_sdio_pio_t *host = bus->context;
    pf_sdio_pio_config_t config = host->config;
    pf_sdio_pio_wait_hook_t wait = host->wait_hook;
    bool initialized = pf_sdio_pio_init(host, &config);
    host->config = config;
    host->wait_hook = wait;
    return initialized ? PF_SD_LINK_OK : PF_SD_LINK_NOT_READY;
}

void pf_sd_link_deinit(const pf_sd_link_t *bus)
{
    if (!pf_sd_link_valid(bus, PF_SD_LINK_NATIVE_SD)) {
        return;
    }
    pf_sdio_pio_t *host = bus->context;
    pf_sdio_pio_config_t config = host->config;
    pf_sdio_pio_wait_hook_t wait = host->wait_hook;
    pf_sdio_pio_deinit(host);
    host->config = config;
    host->wait_hook = wait;
}

void pf_sd_link_abort(const pf_sd_link_t *bus)
{
    if (pf_sd_link_valid(bus, PF_SD_LINK_NATIVE_SD)) {
        pf_sdio_pio_abort(bus->context);
    }
}

pf_sd_link_result_t pf_sd_link_set_clock(const pf_sd_link_t *bus, uint32_t requested, uint32_t *actual)
{
    if (!pf_sd_link_valid(bus, PF_SD_LINK_NATIVE_SD) || requested == 0u || actual == NULL) {
        return PF_SD_LINK_INVALID_ARGUMENT;
    }
    pf_sdio_pio_t *host = bus->context;
    if (!pf_sdio_pio_set_clock(host, requested)) {
        return PF_SD_LINK_IO_ERROR;
    }
    *actual = host->current_clock_hz;
    return PF_SD_LINK_OK;
}

uint64_t pf_sd_link_time_us(const pf_sd_link_t *bus)
{
    (void)bus;
    return time_us_64();
}

void pf_sd_link_delay_ms(const pf_sd_link_t *bus, uint32_t duration)
{
    (void)bus;
    sleep_ms(duration);
}

bool pf_sd_link_enable_wait(const pf_sd_link_t *bus)
{
#if PF_CONFIG_SDCARD_INTERRUPT_WAIT
    return pf_sd_link_valid(bus, PF_SD_LINK_NATIVE_SD) && pf_sdio_pio_enable_wait(bus->context);
#else
    (void)bus;
    return false;
#endif
}

bool pf_sd_link_sdio_valid(const pf_sd_link_t *bus)
{
    return pf_sd_link_valid(bus, PF_SD_LINK_NATIVE_SD);
}

pf_sd_link_sdio_state_t pf_sd_link_sdio_state(const pf_sd_link_t *bus)
{
    if (!pf_sd_link_sdio_valid(bus)) {
        return (pf_sd_link_sdio_state_t){0};
    }
    pf_sdio_pio_t *host = bus->context;
    return (pf_sd_link_sdio_state_t){
        .ready = host->pio != NULL,
        .healthy = host->pio != NULL && !host->mode_restore_failed,
        .data_high = host->pio != NULL && gpio_get(host->config.pins.data_gpio[0]) != 0u,
        .current_clock_hz = host->current_clock_hz,
        .maximum_clock_hz = maximum_clock_hz(clock_get_hz(clk_sys)),
    };
}

bool pf_sd_link_sdio_hooks(const pf_sd_link_t *bus,
                          pf_sd_link_sdio_error_hook_t error, void *context, void (*idle)(void))
{
    if (!pf_sd_link_sdio_valid(bus)) {
        return false;
    }
    pf_sdio_pio_t *host = bus->context;
    host->error_hook = error;
    host->hook_context = context;
    host->idle_hook = idle;
    return true;
}

bool pf_sd_link_sdio_clocks(const pf_sd_link_t *bus, uint32_t cycles, uint32_t timeout_us)
{
    return pf_sd_link_sdio_valid(bus) && cycles != 0u && timeout_us != 0u &&
           pf_sdio_pio_clocks(bus->context, cycles, timeout_us);
}

bool pf_sd_link_sdio_idle(const pf_sd_link_t *bus)
{
    return pf_sd_link_sdio_valid(bus) && pf_sdio_pio_restore_command(bus->context);
}

bool pf_sd_link_sdio_exchange(const pf_sd_link_t *bus, const uint8_t *tx, size_t tx_size,
                                uint16_t rx_bits, uint8_t *rx, size_t capacity, size_t *received,
                                uint32_t timeout_us)
{
    if (received != NULL) {
        *received = 0u;
    }
    return pf_sd_link_sdio_valid(bus) && tx != NULL && tx_size != 0u &&
           rx_bits % 8u == 0u && (rx_bits == 0u || (rx != NULL && capacity >= rx_bits / 8u)) &&
           received != NULL && timeout_us != 0u &&
           pf_sdio_pio_exchange(bus->context, tx, tx_size, rx_bits, rx, capacity, received, timeout_us);
}

bool pf_sd_link_sdio_rx_start(const pf_sd_link_t *bus, uint32_t *buffer,
                                uint32_t words, uint32_t nibbles, uint32_t blocks)
{
    return pf_sd_link_sdio_valid(bus) && buffer != NULL && words != 0u && nibbles != 0u &&
           blocks != 0u && pf_sdio_pio_rx_start(bus->context, buffer, words, nibbles, blocks);
}

bool pf_sd_link_sdio_rx_finish(const pf_sd_link_t *bus, uint32_t timeout_us)
{
    return pf_sd_link_sdio_valid(bus) && timeout_us != 0u &&
           pf_sdio_pio_rx_finish(bus->context, timeout_us);
}

void pf_sd_link_sdio_rx_cancel(const pf_sd_link_t *bus)
{
    if (pf_sd_link_sdio_valid(bus)) {
        pf_sdio_pio_rx_cleanup(bus->context);
    }
}

bool pf_sd_link_sdio_rx_poll(const pf_sd_link_t *bus, uint32_t start,
                               uint32_t timeout_us, bool *complete)
{
    return pf_sd_link_sdio_valid(bus) && complete != NULL && timeout_us != 0u &&
           pf_sdio_pio_rx_poll(bus->context, start, timeout_us, complete);
}

void pf_sd_link_sdio_rx_wait(const pf_sd_link_t *bus)
{
    if (pf_sd_link_sdio_valid(bus)) {
        pf_sdio_pio_rx_wait(bus->context);
    }
}

bool pf_sd_link_sdio_rx_progress(const pf_sd_link_t *bus,
                                   const uint32_t *buffer, uint32_t capacity, uint32_t *words)
{
    return pf_sd_link_sdio_valid(bus) && buffer != NULL && words != NULL && capacity != 0u &&
           pf_sdio_pio_rx_progress(bus->context, buffer, capacity, words);
}

bool pf_sd_link_sdio_tx_start(const pf_sd_link_t *bus,
                                const uint32_t *buffer, uint32_t words, uint32_t nibbles)
{
    if (!pf_sd_link_sdio_valid(bus) || buffer == NULL || words == 0u || nibbles == 0u) {
        return false;
    }
    pf_sdio_pio_t *host = bus->context;
    return (host->clock_owner == PF_SDIO_PIO_CLOCK_DATA_TX || pf_sdio_pio_tx_prepare(host)) &&
           pf_sdio_pio_tx_start(host, buffer, words, nibbles);
}

bool pf_sd_link_sdio_tx_finish(const pf_sd_link_t *bus, uint32_t timeout_us, uint32_t *response)
{
    return pf_sd_link_sdio_valid(bus) && timeout_us != 0u && response != NULL &&
           pf_sdio_pio_tx_finish(bus->context, timeout_us, response);
}

bool pf_sd_link_sdio_wait_high(const pf_sd_link_t *bus, uint32_t timeout_us)
{
    return pf_sd_link_sdio_valid(bus) && timeout_us != 0u &&
           pf_sdio_pio_wait_ready(bus->context, timeout_us);
}

pf_sd_link_t pf_sdio_pio_bus(pf_sdio_pio_t *host)
{
    return (pf_sd_link_t){.context = host, .mode = PF_SD_LINK_NATIVE_SD};
}
