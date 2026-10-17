/** @file Shared mode-0 SPI ownership, pin setup, chip select and byte I/O. */
#include "storage_spi.h"

#include "pico/stdlib.h"

/** One SDK controller's shared wiring and currently selected device. */
typedef struct {
    uint32_t clock_gpio;         /**< Wiring retained while references exist. */
    uint32_t mosi_gpio;          /**< Shared output pin. */
    uint32_t miso_gpio;          /**< Shared input pin. */
    size_t references;          /**< Initialized device contexts. */
    pf_storage_spi_t *selected;  /**< Active CS owner, or NULL between transactions. */
} controller_t;

static controller_t controllers[2];

bool pf_storage_spi_config_valid(const pf_storage_spi_t *context)
{
    if (context == NULL || (context->instance != spi0 && context->instance != spi1) ||
        context->clock_hz == 0u) {
        return false;
    }
    uint32_t pins[] = {context->pins.clock_gpio, context->pins.mosi_gpio, context->pins.miso_gpio, context->pins.cs_gpio};
    for (size_t index = 0u; index < 4u; index++) {
        if (pins[index] >= NUM_BANK0_GPIOS) {
            return false;
        }
        for (size_t previous = 0u; previous < index; previous++) {
            if (pins[index] == pins[previous]) {
                return false;
            }
        }
    }
    uint32_t controller = spi_get_index(context->instance);
    return context->pins.clock_gpio % 4u == 2u && context->pins.mosi_gpio % 4u == 3u &&
           context->pins.miso_gpio % 4u == 0u && (context->pins.clock_gpio / 8u) % 2u == controller &&
           (context->pins.mosi_gpio / 8u) % 2u == controller && (context->pins.miso_gpio / 8u) % 2u == controller;
}

pf_storage_bus_result_t pf_storage_spi_init(pf_storage_spi_t *context)
{
    if (!pf_storage_spi_config_valid(context) || context->initialized) {
        return PF_STORAGE_BUS_INVALID_ARGUMENT;
    }
    controller_t *controller = &controllers[spi_get_index(context->instance)];
    if (controller->selected != NULL || (controller->references != 0u &&
        (controller->clock_gpio != context->pins.clock_gpio || controller->mosi_gpio != context->pins.mosi_gpio ||
         controller->miso_gpio != context->pins.miso_gpio))) {
        return PF_STORAGE_BUS_NOT_READY;
    }
    gpio_init(context->pins.cs_gpio);
    gpio_put(context->pins.cs_gpio, true);
    gpio_set_dir(context->pins.cs_gpio, GPIO_OUT);
    if (controller->references == 0u) {
        if (spi_init(context->instance, context->clock_hz) == 0u) {
            return PF_STORAGE_BUS_IO_ERROR;
        }
        spi_set_format(context->instance, 8u, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
        gpio_set_function(context->pins.clock_gpio, GPIO_FUNC_SPI);
        gpio_set_function(context->pins.mosi_gpio, GPIO_FUNC_SPI);
        gpio_set_function(context->pins.miso_gpio, GPIO_FUNC_SPI);
        gpio_pull_up(context->pins.miso_gpio);
        controller->clock_gpio = context->pins.clock_gpio;
        controller->mosi_gpio = context->pins.mosi_gpio;
        controller->miso_gpio = context->pins.miso_gpio;
    }
    controller->references++;
    context->initialized = true;
    return PF_STORAGE_BUS_OK;
}

pf_storage_bus_result_t pf_storage_spi_set_clock(pf_storage_spi_t *context, uint32_t requested_hz,
                                                uint32_t *actual_hz)
{
    if (context == NULL || requested_hz == 0u || actual_hz == NULL) {
        return PF_STORAGE_BUS_INVALID_ARGUMENT;
    }
    if (!context->initialized) {
        return PF_STORAGE_BUS_NOT_READY;
    }
    controller_t *controller = &controllers[spi_get_index(context->instance)];
    if (controller->selected != NULL && controller->selected != context) {
        return PF_STORAGE_BUS_NOT_READY;
    }
    uint32_t actual = spi_set_baudrate(context->instance, requested_hz);
    if (actual == 0u) {
        return PF_STORAGE_BUS_IO_ERROR;
    }
    context->clock_hz = requested_hz;
    *actual_hz = actual;
    return PF_STORAGE_BUS_OK;
}

pf_storage_bus_result_t pf_storage_spi_select(pf_storage_spi_t *context, bool selected)
{
    if (context == NULL) {
        return PF_STORAGE_BUS_INVALID_ARGUMENT;
    }
    if (!context->initialized) {
        return PF_STORAGE_BUS_NOT_READY;
    }
    controller_t *controller = &controllers[spi_get_index(context->instance)];
    if (controller->selected != NULL && controller->selected != context) {
        return PF_STORAGE_BUS_NOT_READY;
    }
    if (selected) {
        uint32_t actual;
        pf_storage_bus_result_t result = pf_storage_spi_set_clock(context, context->clock_hz, &actual);
        if (result != PF_STORAGE_BUS_OK) {
            return result;
        }
    }
    gpio_put(context->pins.cs_gpio, !selected);
    controller->selected = selected ? context : NULL;
    return PF_STORAGE_BUS_OK;
}

pf_storage_bus_result_t pf_storage_spi_exchange(pf_storage_spi_t *context, const uint8_t *tx,
                                               uint8_t *rx, size_t length)
{
    if (context == NULL || (length != 0u && tx == NULL && rx == NULL)) {
        return PF_STORAGE_BUS_INVALID_ARGUMENT;
    }
    if (!context->initialized) {
        return PF_STORAGE_BUS_NOT_READY;
    }
    controller_t *controller = &controllers[spi_get_index(context->instance)];
    if (controller->selected != NULL && controller->selected != context) {
        return PF_STORAGE_BUS_NOT_READY;
    }
    if (length == 0u) {
        return PF_STORAGE_BUS_OK;
    }
    if (context->timeout_us != 0u) {
        for (size_t index = 0u; index < length; index++) {
            uint64_t start = time_us_64();
            while (!spi_is_writable(context->instance)) {
                if (time_us_64() - start >= context->timeout_us) {
                    return PF_STORAGE_BUS_TIMEOUT;
                }
            }
            spi_get_hw(context->instance)->dr = tx != NULL ? tx[index] : 0xffu;
            while (!spi_is_readable(context->instance)) {
                if (time_us_64() - start >= context->timeout_us) {
                    return PF_STORAGE_BUS_TIMEOUT;
                }
            }
            uint8_t input = (uint8_t)spi_get_hw(context->instance)->dr;
            if (rx != NULL) {
                rx[index] = input;
            }
        }
        return PF_STORAGE_BUS_OK;
    }
    int count = tx == NULL ? spi_read_blocking(context->instance, 0xffu, rx, length) :
                rx == NULL ? spi_write_blocking(context->instance, tx, length) :
                             spi_write_read_blocking(context->instance, tx, rx, length);
    if (count < 0 || (size_t)count != length) {
        return PF_STORAGE_BUS_IO_ERROR;
    }
    return PF_STORAGE_BUS_OK;
}

pf_storage_bus_result_t pf_storage_spi_transfer(pf_storage_spi_t *context,
    const pf_storage_serial_transaction_t *transaction)
{
    if (!pf_storage_serial_transaction_valid(transaction) || transaction->dummy_cycles % 8u != 0u) {
        return PF_STORAGE_BUS_INVALID_ARGUMENT;
    }
    if (transaction->data_lanes != 1u) {
        return PF_STORAGE_BUS_UNSUPPORTED;
    }
    pf_storage_bus_result_t result = pf_storage_spi_select(context, true);
    if (result != PF_STORAGE_BUS_OK) {
        return result;
    }
    busy_wait_us_32(1u);
    result = pf_storage_spi_exchange(context, &transaction->command, NULL, 1u);
    for (uint8_t index = transaction->address_bytes; result == PF_STORAGE_BUS_OK && index != 0u; index--) {
        uint8_t value = (uint8_t)(transaction->address >> ((index - 1u) * 8u));
        result = pf_storage_spi_exchange(context, &value, NULL, 1u);
    }
    for (uint8_t index = 0u; result == PF_STORAGE_BUS_OK && index < transaction->dummy_cycles / 8u; index++) {
        uint8_t value = 0xffu;
        result = pf_storage_spi_exchange(context, &value, NULL, 1u);
    }
    if (result == PF_STORAGE_BUS_OK) {
        result = pf_storage_spi_exchange(context, transaction->tx, transaction->rx, transaction->length);
    }
    (void)pf_storage_spi_select(context, false);
    busy_wait_us_32(1u);
    return result;
}

void pf_storage_spi_deinit(pf_storage_spi_t *context)
{
    if (context == NULL || !context->initialized) {
        return;
    }
    controller_t *controller = &controllers[spi_get_index(context->instance)];
    gpio_put(context->pins.cs_gpio, true);
    if (controller->selected == context) {
        controller->selected = NULL;
    }
    if (--controller->references == 0u) {
        spi_deinit(context->instance);
        gpio_init(context->pins.clock_gpio);
        gpio_init(context->pins.mosi_gpio);
        gpio_init(context->pins.miso_gpio);
    }
    context->initialized = false;
}

static pf_storage_bus_result_t bus_init(void *context) { return pf_storage_spi_init(context); }
static void bus_deinit(void *context) { pf_storage_spi_deinit(context); }
static pf_storage_bus_result_t bus_clock(void *context, uint32_t hz, uint32_t *actual)
{
    return pf_storage_spi_set_clock(context, hz, actual);
}
static pf_storage_bus_result_t bus_transfer(void *context, const pf_storage_serial_transaction_t *transaction)
{
    return pf_storage_spi_transfer(context, transaction);
}

pf_storage_bus_t pf_storage_spi_bus(pf_storage_spi_t *context)
{
    static const pf_storage_bus_ops_t ops = {bus_init, bus_deinit, bus_deinit, bus_clock, bus_transfer};
    return (pf_storage_bus_t){context, &ops, PF_STORAGE_BUS_SPI};
}
