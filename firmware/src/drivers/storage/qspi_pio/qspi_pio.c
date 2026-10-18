/** @file Device-independent PIO single/quad serial signaling. */
#include "qspi_pio.h"

#include "hardware/clocks.h"
#include "qspi_pio.pio.h"
#include "pico/stdlib.h"

/** @brief Round the four-instruction clock divider up so the requested rate is never exceeded. */
static uint64_t clock_divider(uint32_t requested_hz)
{
    return ((uint64_t)clock_get_hz(clk_sys) * 64u + requested_hz - 1u) / requested_hz;
}

/** @brief Restart the state machine with the selected lane count and pin ownership. */
static void configure(pf_qspi_pio_t *context, bool quad, bool receive, uint8_t bits)
{
    pio_sm_set_enabled(context->pio, context->sm, false);
    uint32_t offset = quad ? context->quad_offset : context->single_offset;
    pio_sm_config config = quad ? pf_qspi_pio_quad_program_get_default_config(offset) :
                                 pf_qspi_pio_single_program_get_default_config(offset);
    sm_config_set_out_pins(&config, context->pins.data_gpio[0], quad ? 4u : 1u);
    sm_config_set_in_pins(&config, context->pins.data_gpio[0] + (quad ? 0u : 1u));
    sm_config_set_sideset_pins(&config, context->pins.clock_gpio);
    sm_config_set_out_shift(&config, false, true, bits);
    sm_config_set_in_shift(&config, false, true, bits);
    sm_config_set_clkdiv(&config, (float)clock_divider(context->clock_hz) / 256.0f);
    pio_sm_init(context->pio, context->sm, offset, &config);
    pio_sm_set_pins_with_mask(context->pio, context->sm, 0u, 1u << context->pins.clock_gpio);
    uint32_t data_mask = 15u << context->pins.data_gpio[0];
    uint32_t output_mask = quad ? (receive ? 0u : data_mask) : (1u << context->pins.data_gpio[0]);
    pio_sm_set_pindirs_with_mask(context->pio, context->sm,
                               output_mask | (1u << context->pins.clock_gpio),
                               data_mask | (1u << context->pins.clock_gpio));
    pio_sm_set_enabled(context->pio, context->sm, true);
}

/** @brief Pipeline bytes (or partial dummy symbols), bounding outstanding RX words to FIFO capacity. */
static pf_storage_bus_result_t exchange(pf_qspi_pio_t *context, const pf_storage_serial_phase_t *phase)
{
    bool payload = phase->tx != NULL || phase->rx != NULL;
    uint8_t bits = payload ? 8u : phase->lanes;
    size_t count = phase->bit_count / bits;
    size_t sent = 0u, received = 0u;
    uint64_t stall_start = 0u;
    bool stalled = false;
    configure(context, phase->lanes == 4u, phase->rx != NULL, bits);
    while (received < count) {
        bool progress = false;
        if (sent < count && sent - received < 4u && !pio_sm_is_tx_fifo_full(context->pio, context->sm)) {
            uint8_t output = phase->tx != NULL ? phase->tx[sent] : 0xffu;
            pio_sm_put(context->pio, context->sm, (uint32_t)output << (32u - bits));
            ++sent;
            progress = true;
        }
        if (received < sent && !pio_sm_is_rx_fifo_empty(context->pio, context->sm)) {
            uint8_t input = (uint8_t)pio_sm_get(context->pio, context->sm);
            if (phase->rx != NULL) {
                phase->rx[received] = input;
            }
            ++received;
            progress = true;
        }
        if (progress) {
            stalled = false;
        } else if (!stalled) {
            stall_start = time_us_64();
            stalled = true;
        } else if (time_us_64() - stall_start >= 100000u) {
            return PF_STORAGE_BUS_TIMEOUT;
        }
    }
    return PF_STORAGE_BUS_OK;
}

pf_storage_bus_result_t pf_qspi_pio_transfer(pf_qspi_pio_t *context,
    const pf_storage_serial_phase_t *phases, size_t count)
{
    if (context == NULL) {
        return PF_STORAGE_BUS_INVALID_ARGUMENT;
    }
    pf_storage_bus_result_t result = pf_storage_serial_phases_valid(phases, count, 4u);
    if (result != PF_STORAGE_BUS_OK) {
        return result;
    }
    if (!context->initialized) {
        return PF_STORAGE_BUS_NOT_READY;
    }
    gpio_put(context->pins.cs_gpio, false);
    busy_wait_us_32(1u);
    for (size_t phase_index = 0u; result == PF_STORAGE_BUS_OK && phase_index < count; ++phase_index) {
        result = exchange(context, &phases[phase_index]);
    }
    pio_sm_set_enabled(context->pio, context->sm, false);
    pio_sm_set_pins_with_mask(context->pio, context->sm, 0u, 1u << context->pins.clock_gpio);
    gpio_put(context->pins.cs_gpio, true);
    busy_wait_us_32(1u);
    if (result == PF_STORAGE_BUS_OK) {
        configure(context, false, false, 8u);
    } else {
        pf_qspi_pio_deinit(context);
    }
    return result;
}

pf_storage_bus_result_t pf_qspi_pio_init(pf_qspi_pio_t *context)
{
    if (context == NULL || context->initialized ||
        (context->pio != pio0 && context->pio != pio1) ||
        context->pins.data_gpio[0] > 28u || context->pins.data_gpio[0] + 3u >= NUM_BANK0_GPIOS ||
        context->pins.clock_gpio >= 32u || context->pins.clock_gpio >= NUM_BANK0_GPIOS ||
        context->pins.cs_gpio >= NUM_BANK0_GPIOS || context->pins.cs_gpio == context->pins.clock_gpio ||
        (context->pins.clock_gpio >= context->pins.data_gpio[0] && context->pins.clock_gpio <= context->pins.data_gpio[0] + 3u) ||
        (context->pins.cs_gpio >= context->pins.data_gpio[0] && context->pins.cs_gpio <= context->pins.data_gpio[0] + 3u) ||
        context->clock_hz == 0u || context->clock_hz > 50000000u) {
        return PF_STORAGE_BUS_INVALID_ARGUMENT;
    }
    uint64_t divider = clock_divider(context->clock_hz);
    if (divider < 256u || divider >= 65536u * 256u) {
        return PF_STORAGE_BUS_INVALID_ARGUMENT;
    }
    for (uint32_t lane = 1u; lane < 4u; ++lane) {
        if (context->pins.data_gpio[lane] != context->pins.data_gpio[0] + lane) {
            return PF_STORAGE_BUS_INVALID_ARGUMENT;
        }
    }
    int sm = pio_claim_unused_sm(context->pio, false);
    if (sm < 0) {
        return PF_STORAGE_BUS_NOT_READY;
    }
    if (!pio_can_add_program(context->pio, &pf_qspi_pio_single_program)) {
        pio_sm_unclaim(context->pio, (uint)sm);
        return PF_STORAGE_BUS_NOT_READY;
    }
    context->single_offset = pio_add_program(context->pio, &pf_qspi_pio_single_program);
    if (!pio_can_add_program(context->pio, &pf_qspi_pio_quad_program)) {
        pio_remove_program(context->pio, &pf_qspi_pio_single_program, context->single_offset);
        pio_sm_unclaim(context->pio, (uint)sm);
        return PF_STORAGE_BUS_NOT_READY;
    }
    context->quad_offset = pio_add_program(context->pio, &pf_qspi_pio_quad_program);
    context->sm = (uint32_t)sm;
    gpio_init(context->pins.cs_gpio);
    gpio_put(context->pins.cs_gpio, true);
    gpio_set_dir(context->pins.cs_gpio, GPIO_OUT);
    pio_gpio_init(context->pio, context->pins.clock_gpio);
    for (uint32_t lane = 0u; lane < 4u; lane++) {
        pio_gpio_init(context->pio, context->pins.data_gpio[0] + lane);
    }
    uint32_t data_mask = 15u << context->pins.data_gpio[0];
    context->saved_sync_bypass = context->pio->input_sync_bypass & data_mask;
    /* Clocked NOR data needs the half-cycle setup window, not two extra system-clock input delays. */
    pio_set_input_sync_bypass_with_mask(context->pio, data_mask, data_mask);
    context->initialized = true;
    configure(context, false, false, 8u);
    return PF_STORAGE_BUS_OK;
}

void pf_qspi_pio_deinit(pf_qspi_pio_t *context)
{
    if (context == NULL || !context->initialized) {
        return;
    }
    gpio_put(context->pins.cs_gpio, true);
    pio_sm_set_enabled(context->pio, context->sm, false);
    pio_sm_clear_fifos(context->pio, context->sm);
    pio_sm_unclaim(context->pio, context->sm);
    pio_remove_program(context->pio, &pf_qspi_pio_single_program, context->single_offset);
    pio_remove_program(context->pio, &pf_qspi_pio_quad_program, context->quad_offset);
    pio_set_input_sync_bypass_with_mask(context->pio, context->saved_sync_bypass,
                                      15u << context->pins.data_gpio[0]);
    gpio_init(context->pins.clock_gpio);
    for (uint32_t lane = 0u; lane < 4u; lane++) {
        gpio_init(context->pins.data_gpio[0] + lane);
    }
    context->initialized = false;
}

pf_storage_bus_result_t pf_qspi_pio_set_clock(pf_qspi_pio_t *context,
                                                    uint32_t requested_hz, uint32_t *actual_hz)
{
    if (context == NULL || actual_hz == NULL || requested_hz == 0u || requested_hz > 50000000u) {
        return PF_STORAGE_BUS_INVALID_ARGUMENT;
    }
    if (!context->initialized) {
        return PF_STORAGE_BUS_NOT_READY;
    }
    uint32_t system_hz = clock_get_hz(clk_sys);
    uint64_t divider_256 = clock_divider(requested_hz);
    if (divider_256 < 256u || divider_256 >= 65536u * 256u) {
        return PF_STORAGE_BUS_INVALID_ARGUMENT;
    }
    context->clock_hz = requested_hz;
    configure(context, false, false, 8u);
    *actual_hz = (uint32_t)(((uint64_t)system_hz * 64u) / divider_256);
    return PF_STORAGE_BUS_OK;
}

static pf_storage_bus_result_t bus_init(void *context) { return pf_qspi_pio_init(context); }
static void bus_deinit(void *context) { pf_qspi_pio_deinit(context); }
static pf_storage_bus_result_t bus_clock(void *context, uint32_t hz, uint32_t *actual)
{
    return pf_qspi_pio_set_clock(context, hz, actual);
}
static pf_storage_bus_result_t bus_transfer(void *context, const pf_storage_serial_phase_t *phases, size_t count)
{
    return pf_qspi_pio_transfer(context, phases, count);
}

pf_storage_bus_t pf_qspi_pio_bus(pf_qspi_pio_t *context)
{
    static const pf_storage_serial_ops_t serial = {.transfer = bus_transfer, .lanes = 4u};
    static const pf_storage_bus_ops_t ops = {
        .init = bus_init, .deinit = bus_deinit, .abort = bus_deinit, .set_clock = bus_clock,
        .serial = &serial, .time_us = time_us_64, .delay_ms = sleep_ms,
    };
    return (pf_storage_bus_t){ .context = context, .ops = &ops, .mode = PF_STORAGE_BUS_QSPI };
}
