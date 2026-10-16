#include "nor_spi.h"

#include "pico/stdlib.h"

static uint64_t now(void *context)
{
    (void)context;
    return time_us_64();
}

static pf_nor_result_t exchange(pf_nor_spi_t *context, uint8_t tx, uint8_t *rx)
{
    uint64_t start = time_us_64();
    while (!spi_is_writable(context->instance)) {
        if (time_us_64() - start >= 100000u) {
            return PF_NOR_TIMEOUT;
        }
    }
    spi_get_hw(context->instance)->dr = tx;
    while (!spi_is_readable(context->instance)) {
        if (time_us_64() - start >= 100000u) {
            return PF_NOR_TIMEOUT;
        }
    }
    *rx = (uint8_t)spi_get_hw(context->instance)->dr;
    return PF_NOR_OK;
}

static pf_nor_result_t execute(void *opaque, const pf_nor_transaction_t *transaction)
{
    pf_nor_spi_t *context = opaque;
    if (!pf_nor_transaction_valid(transaction) || transaction->dummy_cycles % 8u != 0u) {
        return PF_NOR_INVALID_ARGUMENT;
    }
    if (!context->initialized) {
        return PF_NOR_NOT_READY;
    }
    if (transaction->data_lanes != 1u) {
        return PF_NOR_UNSUPPORTED;
    }
    uint8_t ignored;
    gpio_put(context->cs_gpio, false);
    busy_wait_us_32(1u);
    pf_nor_result_t result = exchange(context, transaction->command, &ignored);
    for (uint8_t index = transaction->address_bytes; result == PF_NOR_OK && index != 0u; index--) {
        result = exchange(context, (uint8_t)(transaction->address >> ((index - 1u) * 8u)), &ignored);
    }
    for (uint8_t index = 0u; result == PF_NOR_OK && index < transaction->dummy_cycles / 8u; index++) {
        result = exchange(context, 0xffu, &ignored);
    }
    for (size_t index = 0u; result == PF_NOR_OK && index < transaction->length; index++) {
        result = exchange(context, transaction->tx != NULL ? transaction->tx[index] : 0xffu, &ignored);
        if (result == PF_NOR_OK && transaction->rx != NULL) {
            transaction->rx[index] = ignored;
        }
    }
    gpio_put(context->cs_gpio, true);
    busy_wait_us_32(1u);
    if (result != PF_NOR_OK) {
        pf_nor_spi_deinit(context);
    }
    return result;
}

pf_nor_result_t pf_nor_spi_init(pf_nor_spi_t *context, pf_nor_transport_t *transport)
{
    if (context == NULL || transport == NULL || context->initialized ||
        (context->instance != spi0 && context->instance != spi1) ||
        context->clock_hz == 0u || context->clock_hz > 10000000u) {
        return PF_NOR_INVALID_ARGUMENT;
    }
    uint32_t pins[] = {context->clock_gpio, context->mosi_gpio, context->miso_gpio,
                       context->wp_gpio, context->hold_gpio, context->cs_gpio};
    for (size_t index = 0u; index < 6u; index++) {
        if (pins[index] >= NUM_BANK0_GPIOS) {
            return PF_NOR_INVALID_ARGUMENT;
        }
        for (size_t previous = 0u; previous < index; previous++) {
            if (pins[index] == pins[previous]) {
                return PF_NOR_INVALID_ARGUMENT;
            }
        }
    }
    uint32_t controller = context->instance == spi0 ? 0u : 1u;
    if ((context->clock_gpio % 4u != 2u) || (context->mosi_gpio % 4u != 3u) ||
        (context->miso_gpio % 4u != 0u) ||
        ((context->clock_gpio / 8u) % 2u != controller) ||
        ((context->mosi_gpio / 8u) % 2u != controller) ||
        ((context->miso_gpio / 8u) % 2u != controller)) {
        return PF_NOR_INVALID_ARGUMENT;
    }
    gpio_init(context->cs_gpio);
    gpio_put(context->cs_gpio, true);
    gpio_set_dir(context->cs_gpio, GPIO_OUT);
    gpio_init(context->wp_gpio);
    gpio_put(context->wp_gpio, true);
    gpio_set_dir(context->wp_gpio, GPIO_OUT);
    gpio_init(context->hold_gpio);
    gpio_put(context->hold_gpio, true);
    gpio_set_dir(context->hold_gpio, GPIO_OUT);
    spi_init(context->instance, context->clock_hz);
    spi_set_format(context->instance, 8u, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(context->clock_gpio, GPIO_FUNC_SPI);
    gpio_set_function(context->mosi_gpio, GPIO_FUNC_SPI);
    gpio_set_function(context->miso_gpio, GPIO_FUNC_SPI);
    context->initialized = true;
    *transport = (pf_nor_transport_t){ .context = context, .execute = execute, .time_us = now };
    return PF_NOR_OK;
}

void pf_nor_spi_deinit(pf_nor_spi_t *context)
{
    if (context == NULL || !context->initialized) {
        return;
    }
    gpio_put(context->cs_gpio, true);
    spi_deinit(context->instance);
    uint32_t pins[] = {context->clock_gpio, context->mosi_gpio, context->miso_gpio,
                       context->wp_gpio, context->hold_gpio};
    for (size_t index = 0u; index < 5u; index++) {
        gpio_init(pins[index]);
    }
    context->initialized = false;
}
