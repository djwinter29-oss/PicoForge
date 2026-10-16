#include "nor_qspi_pio.h"

#include "hardware/clocks.h"
#include "nor_qspi.pio.h"
#include "pico/stdlib.h"

static uint64_t now(void *context)
{
    (void)context;
    return time_us_64();
}

static void configure(pf_nor_qspi_pio_t *context, bool quad, bool receive)
{
    pio_sm_set_enabled(context->pio, context->sm, false);
    uint32_t offset = quad ? context->quad_offset : context->single_offset;
    pio_sm_config config = quad ? pf_nor_quad_program_get_default_config(offset) :
                                 pf_nor_single_program_get_default_config(offset);
    sm_config_set_out_pins(&config, context->data_gpio, quad ? 4u : 1u);
    sm_config_set_in_pins(&config, context->data_gpio + (quad ? 0u : 1u));
    sm_config_set_sideset_pins(&config, context->clock_gpio);
    sm_config_set_out_shift(&config, false, false, 32u);
    sm_config_set_in_shift(&config, false, false, 32u);
    float divider = (float)clock_get_hz(clk_sys) / (4.0f * (float)context->clock_hz);
    sm_config_set_clkdiv(&config, divider);
    pio_sm_init(context->pio, context->sm, offset, &config);
    pio_sm_set_pins_with_mask(context->pio, context->sm, 0u, 1u << context->clock_gpio);
    uint32_t data_mask = 15u << context->data_gpio;
    uint32_t output_mask = quad ? (receive ? 0u : data_mask) : (1u << context->data_gpio);
    pio_sm_set_pindirs_with_mask(context->pio, context->sm,
                               output_mask | (1u << context->clock_gpio),
                               data_mask | (1u << context->clock_gpio));
    pio_sm_set_enabled(context->pio, context->sm, true);
}

static pf_nor_result_t symbol(pf_nor_qspi_pio_t *context, uint8_t output,
                               uint8_t bits, uint8_t *input)
{
    uint64_t start = time_us_64();
    while (pio_sm_is_tx_fifo_full(context->pio, context->sm)) {
        if (time_us_64() - start >= 100000u) {
            return PF_NOR_TIMEOUT;
        }
    }
    pio_sm_put(context->pio, context->sm, (uint32_t)output << (32u - bits));
    while (pio_sm_is_rx_fifo_empty(context->pio, context->sm)) {
        if (time_us_64() - start >= 100000u) {
            return PF_NOR_TIMEOUT;
        }
    }
    *input = (uint8_t)(pio_sm_get(context->pio, context->sm) & ((1u << bits) - 1u));
    return PF_NOR_OK;
}

static pf_nor_result_t byte(pf_nor_qspi_pio_t *context, uint8_t output,
                             uint8_t lanes, uint8_t *input)
{
    *input = 0u;
    for (int shift = 8 - lanes; shift >= 0; shift -= lanes) {
        uint8_t received;
        pf_nor_result_t result = symbol(context, (uint8_t)(output >> shift), lanes, &received);
        if (result != PF_NOR_OK) {
            return result;
        }
        *input = (uint8_t)((*input << lanes) | received);
    }
    return PF_NOR_OK;
}

static pf_nor_result_t execute(void *opaque, const pf_nor_transaction_t *transaction)
{
    pf_nor_qspi_pio_t *context = opaque;
    if (!pf_nor_transaction_valid(transaction)) {
        return PF_NOR_INVALID_ARGUMENT;
    }
    if (!context->initialized) {
        return PF_NOR_NOT_READY;
    }
    configure(context, false, false);
    gpio_put(context->cs_gpio, false);
    busy_wait_us_32(1u);
    uint8_t ignored;
    pf_nor_result_t result = byte(context, transaction->command, 1u, &ignored);
    for (uint8_t index = transaction->address_bytes; result == PF_NOR_OK && index != 0u; index--) {
        result = byte(context, (uint8_t)(transaction->address >> ((index - 1u) * 8u)), 1u, &ignored);
    }
    for (uint8_t index = 0u; result == PF_NOR_OK && index < transaction->dummy_cycles; index++) {
        result = symbol(context, 1u, 1u, &ignored);
    }
    if (result == PF_NOR_OK && transaction->data_lanes == 4u) {
        configure(context, true, transaction->rx != NULL);
    }
    for (size_t index = 0u; result == PF_NOR_OK && index < transaction->length; index++) {
        result = byte(context, transaction->tx != NULL ? transaction->tx[index] : 0xffu,
                      transaction->data_lanes, &ignored);
        if (result == PF_NOR_OK && transaction->rx != NULL) {
            transaction->rx[index] = ignored;
        }
    }
    gpio_put(context->cs_gpio, true);
    busy_wait_us_32(1u);
    if (result == PF_NOR_OK) {
        configure(context, false, false);
    } else {
        pf_nor_qspi_pio_deinit(context);
    }
    return result;
}

pf_nor_result_t pf_nor_qspi_pio_init(pf_nor_qspi_pio_t *context, pf_nor_transport_t *transport)
{
    if (context == NULL || transport == NULL || context->initialized ||
        (context->pio != pio0 && context->pio != pio1) ||
        context->data_gpio > 28u || context->data_gpio + 3u >= NUM_BANK0_GPIOS ||
        context->clock_gpio >= 32u || context->clock_gpio >= NUM_BANK0_GPIOS ||
        context->cs_gpio >= NUM_BANK0_GPIOS || context->cs_gpio == context->clock_gpio ||
        (context->clock_gpio >= context->data_gpio && context->clock_gpio <= context->data_gpio + 3u) ||
        (context->cs_gpio >= context->data_gpio && context->cs_gpio <= context->data_gpio + 3u) ||
        context->clock_hz == 0u || context->clock_hz > 10000000u) {
        return PF_NOR_INVALID_ARGUMENT;
    }
    float divider = (float)clock_get_hz(clk_sys) / (4.0f * (float)context->clock_hz);
    if (divider < 1.0f || divider >= 65536.0f) {
        return PF_NOR_INVALID_ARGUMENT;
    }
    int sm = pio_claim_unused_sm(context->pio, false);
    if (sm < 0) {
        return PF_NOR_NOT_READY;
    }
    if (!pio_can_add_program(context->pio, &pf_nor_single_program)) {
        pio_sm_unclaim(context->pio, (uint)sm);
        return PF_NOR_NOT_READY;
    }
    context->single_offset = pio_add_program(context->pio, &pf_nor_single_program);
    if (!pio_can_add_program(context->pio, &pf_nor_quad_program)) {
        pio_remove_program(context->pio, &pf_nor_single_program, context->single_offset);
        pio_sm_unclaim(context->pio, (uint)sm);
        return PF_NOR_NOT_READY;
    }
    context->quad_offset = pio_add_program(context->pio, &pf_nor_quad_program);
    context->sm = (uint32_t)sm;
    gpio_init(context->cs_gpio);
    gpio_put(context->cs_gpio, true);
    gpio_set_dir(context->cs_gpio, GPIO_OUT);
    pio_gpio_init(context->pio, context->clock_gpio);
    for (uint32_t lane = 0u; lane < 4u; lane++) {
        pio_gpio_init(context->pio, context->data_gpio + lane);
    }
    context->initialized = true;
    configure(context, false, false);
    *transport = (pf_nor_transport_t){
        .context = context, .execute = execute, .time_us = now, .quad_supported = true,
    };
    return PF_NOR_OK;
}

void pf_nor_qspi_pio_deinit(pf_nor_qspi_pio_t *context)
{
    if (context == NULL || !context->initialized) {
        return;
    }
    gpio_put(context->cs_gpio, true);
    pio_sm_set_enabled(context->pio, context->sm, false);
    pio_sm_clear_fifos(context->pio, context->sm);
    pio_sm_unclaim(context->pio, context->sm);
    pio_remove_program(context->pio, &pf_nor_single_program, context->single_offset);
    pio_remove_program(context->pio, &pf_nor_quad_program, context->quad_offset);
    gpio_init(context->clock_gpio);
    for (uint32_t lane = 0u; lane < 4u; lane++) {
        gpio_init(context->data_gpio + lane);
    }
    context->initialized = false;
}
