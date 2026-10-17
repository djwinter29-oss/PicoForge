/** @file NOR compatibility adapter; command and payload signaling belongs to storage_spi. */
#include "nor_spi.h"

#include "pico/stdlib.h"

static uint64_t now(void *context)
{
    (void)context;
    return time_us_64();
}

static pf_nor_result_t execute(void *opaque, const pf_nor_transaction_t *transaction)
{
    pf_nor_spi_t *context = opaque;
    pf_storage_bus_t bus = pf_storage_spi_bus(&context->bus);
    pf_nor_result_t result = pf_nor_result_from_storage(pf_storage_bus_transfer(&bus, transaction));
    if (result == PF_NOR_IO_ERROR || result == PF_NOR_TIMEOUT) {
        pf_nor_spi_deinit(context);
    }
    return result;
}

pf_nor_result_t pf_nor_spi_init(pf_nor_spi_t *context, pf_nor_transport_t *transport)
{
    if (context == NULL || transport == NULL || context->initialized ||
        context->clock_hz == 0u || context->clock_hz > 10000000u) {
        return PF_NOR_INVALID_ARGUMENT;
    }
    context->bus = (pf_storage_spi_t){
        .instance = context->instance, .clock_gpio = context->clock_gpio,
        .mosi_gpio = context->mosi_gpio, .miso_gpio = context->miso_gpio,
        .cs_gpio = context->cs_gpio, .clock_hz = context->clock_hz, .timeout_us = 100000u,
    };
    uint32_t pins[] = {context->clock_gpio, context->mosi_gpio, context->miso_gpio,
                       context->wp_gpio, context->hold_gpio, context->cs_gpio};
    if (!pf_storage_spi_config_valid(&context->bus)) {
        return PF_NOR_INVALID_ARGUMENT;
    }
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
    pf_storage_bus_t bus = pf_storage_spi_bus(&context->bus);
    pf_nor_result_t result = pf_nor_result_from_storage(pf_storage_bus_init(&bus));
    if (result != PF_NOR_OK) {
        return result;
    }
    gpio_init(context->wp_gpio);
    gpio_put(context->wp_gpio, true);
    gpio_set_dir(context->wp_gpio, GPIO_OUT);
    gpio_init(context->hold_gpio);
    gpio_put(context->hold_gpio, true);
    gpio_set_dir(context->hold_gpio, GPIO_OUT);
    context->initialized = true;
    *transport = (pf_nor_transport_t){ .context = context, .execute = execute, .time_us = now };
    return PF_NOR_OK;
}

void pf_nor_spi_deinit(pf_nor_spi_t *context)
{
    if (context == NULL || !context->initialized) {
        return;
    }
    pf_storage_bus_t bus = pf_storage_spi_bus(&context->bus);
    pf_storage_bus_deinit(&bus);
    gpio_init(context->wp_gpio);
    gpio_init(context->hold_gpio);
    context->initialized = false;
}
