/** @file Device-independent PIO single/quad serial signaling. */
#include "storage_qspi_pio.h"

#include "hardware/clocks.h"
#include "storage_qspi_pio.pio.h"
#include "pico/stdlib.h"

/** @brief Restart the state machine with the selected lane count and pin ownership. */
static void configure(pf_storage_qspi_pio_t *context, bool quad, bool receive)
{
    pio_sm_set_enabled(context->pio, context->sm, false);
    uint32_t offset = quad ? context->quad_offset : context->single_offset;
    pio_sm_config config = quad ? pf_storage_quad_program_get_default_config(offset) :
                                 pf_storage_single_program_get_default_config(offset);
    sm_config_set_out_pins(&config, context->pins.data_gpio[0], quad ? 4u : 1u);
    sm_config_set_in_pins(&config, context->pins.data_gpio[0] + (quad ? 0u : 1u));
    sm_config_set_sideset_pins(&config, context->pins.clock_gpio);
    sm_config_set_out_shift(&config, false, false, 32u);
    sm_config_set_in_shift(&config, false, false, 32u);
    float divider = (float)clock_get_hz(clk_sys) / (4.0f * (float)context->clock_hz);
    sm_config_set_clkdiv(&config, divider);
    pio_sm_init(context->pio, context->sm, offset, &config);
    pio_sm_set_pins_with_mask(context->pio, context->sm, 0u, 1u << context->pins.clock_gpio);
    uint32_t data_mask = 15u << context->pins.data_gpio[0];
    uint32_t output_mask = quad ? (receive ? 0u : data_mask) : (1u << context->pins.data_gpio[0]);
    pio_sm_set_pindirs_with_mask(context->pio, context->sm,
                               output_mask | (1u << context->pins.clock_gpio),
                               data_mask | (1u << context->pins.clock_gpio));
    pio_sm_set_enabled(context->pio, context->sm, true);
}

/** @brief Clock one symbol with a bounded FIFO wait. */
static pf_storage_transport_result_t symbol(pf_storage_qspi_pio_t *context, uint8_t output,
                               uint8_t bits, uint8_t *input)
{
    uint64_t start = time_us_64();
    while (pio_sm_is_tx_fifo_full(context->pio, context->sm)) {
        if (time_us_64() - start >= 100000u) {
            return PF_STORAGE_TRANSPORT_TIMEOUT;
        }
    }
    pio_sm_put(context->pio, context->sm, (uint32_t)output << (32u - bits));
    while (pio_sm_is_rx_fifo_empty(context->pio, context->sm)) {
        if (time_us_64() - start >= 100000u) {
            return PF_STORAGE_TRANSPORT_TIMEOUT;
        }
    }
    *input = (uint8_t)(pio_sm_get(context->pio, context->sm) & ((1u << bits) - 1u));
    return PF_STORAGE_TRANSPORT_OK;
}

/** @brief Exchange one byte, most-significant symbol first. */
static pf_storage_transport_result_t byte(pf_storage_qspi_pio_t *context, uint8_t output,
                             uint8_t lanes, uint8_t *input)
{
    *input = 0u;
    for (int shift = 8 - lanes; shift >= 0; shift -= lanes) {
        uint8_t received;
        pf_storage_transport_result_t result = symbol(context, (uint8_t)(output >> shift), lanes, &received);
        if (result != PF_STORAGE_TRANSPORT_OK) {
            return result;
        }
        *input = (uint8_t)((*input << lanes) | received);
    }
    return PF_STORAGE_TRANSPORT_OK;
}

pf_storage_transport_result_t pf_storage_qspi_pio_transfer(pf_storage_qspi_pio_t *context,
    const pf_storage_serial_transaction_t *transaction)
{
    if (context == NULL || !pf_storage_serial_transaction_valid(transaction)) {
        return PF_STORAGE_TRANSPORT_INVALID_ARGUMENT;
    }
    if (!context->initialized) {
        return PF_STORAGE_TRANSPORT_NOT_READY;
    }
    configure(context, false, false);
    gpio_put(context->pins.cs_gpio, false);
    busy_wait_us_32(1u);
    uint8_t ignored;
    pf_storage_transport_result_t result = byte(context, transaction->command, 1u, &ignored);
    for (uint8_t index = transaction->address_bytes; result == PF_STORAGE_TRANSPORT_OK && index != 0u; index--) {
        result = byte(context, (uint8_t)(transaction->address >> ((index - 1u) * 8u)), 1u, &ignored);
    }
    for (uint8_t index = 0u; result == PF_STORAGE_TRANSPORT_OK && index < transaction->dummy_cycles; index++) {
        result = symbol(context, 1u, 1u, &ignored);
    }
    if (result == PF_STORAGE_TRANSPORT_OK && transaction->data_lanes == 4u) {
        configure(context, true, transaction->rx != NULL);
    }
    for (size_t index = 0u; result == PF_STORAGE_TRANSPORT_OK && index < transaction->length; index++) {
        result = byte(context, transaction->tx != NULL ? transaction->tx[index] : 0xffu,
                      transaction->data_lanes, &ignored);
        if (result == PF_STORAGE_TRANSPORT_OK && transaction->rx != NULL) {
            transaction->rx[index] = ignored;
        }
    }
    gpio_put(context->pins.cs_gpio, true);
    busy_wait_us_32(1u);
    if (result == PF_STORAGE_TRANSPORT_OK) {
        configure(context, false, false);
    } else {
        pf_storage_qspi_pio_deinit(context);
    }
    return result;
}

pf_storage_transport_result_t pf_storage_qspi_pio_init(pf_storage_qspi_pio_t *context)
{
    if (context == NULL || context->initialized ||
        (context->pio != pio0 && context->pio != pio1) ||
        context->pins.data_gpio[0] > 28u || context->pins.data_gpio[0] + 3u >= NUM_BANK0_GPIOS ||
        context->pins.clock_gpio >= 32u || context->pins.clock_gpio >= NUM_BANK0_GPIOS ||
        context->pins.cs_gpio >= NUM_BANK0_GPIOS || context->pins.cs_gpio == context->pins.clock_gpio ||
        (context->pins.clock_gpio >= context->pins.data_gpio[0] && context->pins.clock_gpio <= context->pins.data_gpio[0] + 3u) ||
        (context->pins.cs_gpio >= context->pins.data_gpio[0] && context->pins.cs_gpio <= context->pins.data_gpio[0] + 3u) ||
        context->clock_hz == 0u || context->clock_hz > 10000000u) {
        return PF_STORAGE_TRANSPORT_INVALID_ARGUMENT;
    }
    float divider = (float)clock_get_hz(clk_sys) / (4.0f * (float)context->clock_hz);
    if (divider < 1.0f || divider >= 65536.0f) {
        return PF_STORAGE_TRANSPORT_INVALID_ARGUMENT;
    }
    for (uint32_t lane = 1u; lane < 4u; ++lane) {
        if (context->pins.data_gpio[lane] != context->pins.data_gpio[0] + lane) {
            return PF_STORAGE_TRANSPORT_INVALID_ARGUMENT;
        }
    }
    int sm = pio_claim_unused_sm(context->pio, false);
    if (sm < 0) {
        return PF_STORAGE_TRANSPORT_NOT_READY;
    }
    if (!pio_can_add_program(context->pio, &pf_storage_single_program)) {
        pio_sm_unclaim(context->pio, (uint)sm);
        return PF_STORAGE_TRANSPORT_NOT_READY;
    }
    context->single_offset = pio_add_program(context->pio, &pf_storage_single_program);
    if (!pio_can_add_program(context->pio, &pf_storage_quad_program)) {
        pio_remove_program(context->pio, &pf_storage_single_program, context->single_offset);
        pio_sm_unclaim(context->pio, (uint)sm);
        return PF_STORAGE_TRANSPORT_NOT_READY;
    }
    context->quad_offset = pio_add_program(context->pio, &pf_storage_quad_program);
    context->sm = (uint32_t)sm;
    gpio_init(context->pins.cs_gpio);
    gpio_put(context->pins.cs_gpio, true);
    gpio_set_dir(context->pins.cs_gpio, GPIO_OUT);
    pio_gpio_init(context->pio, context->pins.clock_gpio);
    for (uint32_t lane = 0u; lane < 4u; lane++) {
        pio_gpio_init(context->pio, context->pins.data_gpio[0] + lane);
    }
    context->initialized = true;
    configure(context, false, false);
    return PF_STORAGE_TRANSPORT_OK;
}

void pf_storage_qspi_pio_deinit(pf_storage_qspi_pio_t *context)
{
    if (context == NULL || !context->initialized) {
        return;
    }
    gpio_put(context->pins.cs_gpio, true);
    pio_sm_set_enabled(context->pio, context->sm, false);
    pio_sm_clear_fifos(context->pio, context->sm);
    pio_sm_unclaim(context->pio, context->sm);
    pio_remove_program(context->pio, &pf_storage_single_program, context->single_offset);
    pio_remove_program(context->pio, &pf_storage_quad_program, context->quad_offset);
    gpio_init(context->pins.clock_gpio);
    for (uint32_t lane = 0u; lane < 4u; lane++) {
        gpio_init(context->pins.data_gpio[0] + lane);
    }
    context->initialized = false;
}

pf_storage_transport_result_t pf_storage_qspi_pio_set_clock(pf_storage_qspi_pio_t *context,
                                                    uint32_t requested_hz, uint32_t *actual_hz)
{
    if (context == NULL || actual_hz == NULL || requested_hz == 0u || requested_hz > 10000000u) {
        return PF_STORAGE_TRANSPORT_INVALID_ARGUMENT;
    }
    if (!context->initialized) {
        return PF_STORAGE_TRANSPORT_NOT_READY;
    }
    uint32_t system_hz = clock_get_hz(clk_sys);
    float divider = (float)system_hz / (4.0f * (float)requested_hz);
    if (divider < 1.0f || divider >= 65536.0f) {
        return PF_STORAGE_TRANSPORT_INVALID_ARGUMENT;
    }
    context->clock_hz = requested_hz;
    configure(context, false, false);
    uint32_t divider_256 = (uint32_t)(divider * 256.0f);
    *actual_hz = (uint32_t)(((uint64_t)system_hz * 64u) / divider_256);
    return PF_STORAGE_TRANSPORT_OK;
}

static pf_storage_transport_result_t bus_init(void *context) { return pf_storage_qspi_pio_init(context); }
static void bus_deinit(void *context) { pf_storage_qspi_pio_deinit(context); }
static pf_storage_transport_result_t bus_clock(void *context, uint32_t hz, uint32_t *actual)
{
    return pf_storage_qspi_pio_set_clock(context, hz, actual);
}
static pf_storage_transport_result_t bus_transfer(void *context, const pf_storage_serial_transaction_t *transaction)
{
    return pf_storage_qspi_pio_transfer(context, transaction);
}

pf_storage_transport_t pf_storage_qspi_pio_bus(pf_storage_qspi_pio_t *context)
{
    static const pf_storage_transport_ops_t ops = {
        .init = bus_init, .deinit = bus_deinit, .abort = bus_deinit, .set_clock = bus_clock,
        .transfer = bus_transfer, .time_us = time_us_64, .delay_ms = sleep_ms,
    };
    return (pf_storage_transport_t){ .context = context, .ops = &ops, .mode = PF_STORAGE_TRANSPORT_QSPI_PIO };
}
