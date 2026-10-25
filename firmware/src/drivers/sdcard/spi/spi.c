/** @file Shared mode-0 SPI ownership, pin setup, chip select and byte I/O. */
#include "spi.h"

#include "pico/stdlib.h"

/** One SDK controller's shared wiring and currently selected device. */
typedef struct {
    uint32_t clock_gpio;         /**< Wiring retained while references exist. */
    uint32_t mosi_gpio;          /**< Shared output pin. */
    uint32_t miso_gpio;          /**< Shared input pin. */
    size_t references;          /**< Initialized device contexts. */
    pf_spi_t *selected;  /**< Active CS owner, or NULL between transactions. */
} controller_t;

static controller_t controllers[2];

bool pf_spi_pins_match_controller(uint32_t spi_index, uint32_t clock_gpio,
                                  uint32_t mosi_gpio, uint32_t miso_gpio)
{
    return spi_index <= 1u && clock_gpio % 4u == 2u && mosi_gpio % 4u == 3u &&
           miso_gpio % 4u == 0u && (clock_gpio / 8u) % 2u == spi_index &&
           (mosi_gpio / 8u) % 2u == spi_index && (miso_gpio / 8u) % 2u == spi_index;
}

bool pf_spi_config_valid(const pf_spi_t *context)
{
    if (context == NULL || (context->instance != spi0 && context->instance != spi1) ||
        context->clock_hz == 0u) {
        return false;
    }
    uint32_t pins[] = {
        context->pins.clock_gpio,
        context->pins.mosi_gpio,
        context->pins.miso_gpio,
        context->pins.cs_gpio,
    };
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
    return pf_spi_pins_match_controller(spi_get_index(context->instance), context->pins.clock_gpio,
                                        context->pins.mosi_gpio, context->pins.miso_gpio);
}

pf_sd_link_result_t pf_spi_init(pf_spi_t *context)
{
    if (!pf_spi_config_valid(context) || context->initialized) {
        return PF_SD_LINK_INVALID_ARGUMENT;
    }
    controller_t *controller = &controllers[spi_get_index(context->instance)];
    if (controller->selected != NULL || (controller->references != 0u &&
        (controller->clock_gpio != context->pins.clock_gpio || controller->mosi_gpio != context->pins.mosi_gpio ||
         controller->miso_gpio != context->pins.miso_gpio))) {
        return PF_SD_LINK_NOT_READY;
    }
    gpio_init(context->pins.cs_gpio);
    gpio_put(context->pins.cs_gpio, true);
    gpio_set_dir(context->pins.cs_gpio, GPIO_OUT);
    if (controller->references == 0u) {
        if (spi_init(context->instance, context->clock_hz) == 0u) {
            return PF_SD_LINK_IO_ERROR;
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
    return PF_SD_LINK_OK;
}

pf_sd_link_result_t pf_spi_set_clock(pf_spi_t *context, uint32_t requested_hz,
                                                uint32_t *actual_hz)
{
    if (context == NULL || requested_hz == 0u || actual_hz == NULL) {
        return PF_SD_LINK_INVALID_ARGUMENT;
    }
    if (!context->initialized) {
        return PF_SD_LINK_NOT_READY;
    }
    controller_t *controller = &controllers[spi_get_index(context->instance)];
    if (controller->selected != NULL && controller->selected != context) {
        return PF_SD_LINK_NOT_READY;
    }
    uint32_t actual = spi_set_baudrate(context->instance, requested_hz);
    if (actual == 0u) {
        return PF_SD_LINK_IO_ERROR;
    }
    context->clock_hz = requested_hz;
    *actual_hz = actual;
    return PF_SD_LINK_OK;
}

pf_sd_link_result_t pf_spi_select(pf_spi_t *context, bool selected)
{
    if (context == NULL) {
        return PF_SD_LINK_INVALID_ARGUMENT;
    }
    if (!context->initialized) {
        return PF_SD_LINK_NOT_READY;
    }
    controller_t *controller = &controllers[spi_get_index(context->instance)];
    if (controller->selected != NULL && controller->selected != context) {
        return PF_SD_LINK_NOT_READY;
    }
    if (selected) {
        uint32_t actual;
        pf_sd_link_result_t result = pf_spi_set_clock(context, context->clock_hz, &actual);
        if (result != PF_SD_LINK_OK) {
            return result;
        }
    }
    gpio_put(context->pins.cs_gpio, !selected);
    controller->selected = selected ? context : NULL;
    return PF_SD_LINK_OK;
}

pf_sd_link_result_t pf_spi_exchange(pf_spi_t *context, const uint8_t *tx,
                                               uint8_t *rx, size_t length)
{
    if (context == NULL || (length != 0u && tx == NULL && rx == NULL)) {
        return PF_SD_LINK_INVALID_ARGUMENT;
    }
    if (!context->initialized) {
        return PF_SD_LINK_NOT_READY;
    }
    controller_t *controller = &controllers[spi_get_index(context->instance)];
    if (controller->selected != NULL && controller->selected != context) {
        return PF_SD_LINK_NOT_READY;
    }
    if (length == 0u) {
        return PF_SD_LINK_OK;
    }
    if (context->timeout_us != 0u) {
        size_t sent = 0u, received = 0u;
        uint64_t stalled_at = 0u;
        bool stalled = false;
        while (received < length) {
            bool progress = false;
            /* Match the SDK's eight-byte in-flight limit so an interrupt cannot overflow RX. */
            if (sent < length && sent - received < 8u && spi_is_writable(context->instance)) {
                spi_get_hw(context->instance)->dr = tx != NULL ? tx[sent] : 0xffu;
                ++sent;
                progress = true;
            }
            if (received < sent && spi_is_readable(context->instance)) {
                uint8_t input = (uint8_t)spi_get_hw(context->instance)->dr;
                if (rx != NULL) {
                    rx[received] = input;
                }
                ++received;
                progress = true;
            }
            if (progress) {
                stalled = false;
            } else if (!stalled) {
                stalled_at = time_us_64();
                stalled = true;
            } else if (time_us_64() - stalled_at >= context->timeout_us) {
                return PF_SD_LINK_TIMEOUT;
            }
        }
        return PF_SD_LINK_OK;
    }
    int count = tx == NULL ? spi_read_blocking(context->instance, 0xffu, rx, length) :
                rx == NULL ? spi_write_blocking(context->instance, tx, length) :
                             spi_write_read_blocking(context->instance, tx, rx, length);
    if (count < 0 || (size_t)count != length) {
        return PF_SD_LINK_IO_ERROR;
    }
    return PF_SD_LINK_OK;
}

void pf_spi_deinit(pf_spi_t *context)
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

pf_sd_link_result_t pf_sd_link_init(const pf_sd_link_t *bus)
{
    return pf_sd_link_valid(bus, PF_SD_LINK_SPI) ? pf_spi_init(bus->context) : PF_SD_LINK_INVALID_ARGUMENT;
}

void pf_sd_link_deinit(const pf_sd_link_t *bus)
{
    if (pf_sd_link_valid(bus, PF_SD_LINK_SPI)) {
        pf_spi_deinit(bus->context);
    }
}

void pf_sd_link_abort(const pf_sd_link_t *bus)
{
    pf_sd_link_deinit(bus);
}

pf_sd_link_result_t pf_sd_link_set_clock(const pf_sd_link_t *bus, uint32_t hz, uint32_t *actual)
{
    return pf_sd_link_valid(bus, PF_SD_LINK_SPI) ? pf_spi_set_clock(bus->context, hz, actual) :
                                                PF_SD_LINK_INVALID_ARGUMENT;
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
    (void)bus;
    return false;
}

pf_sd_link_result_t pf_sd_link_serial_select(const pf_sd_link_t *transport, bool selected)
{
    return pf_sd_link_valid(transport, PF_SD_LINK_SPI) ? pf_spi_select(transport->context, selected) :
                                                       PF_SD_LINK_INVALID_ARGUMENT;
}

pf_sd_link_result_t pf_sd_link_serial_exchange(const pf_sd_link_t *transport,
                                              const uint8_t *tx, uint8_t *rx, size_t length)
{
    return pf_sd_link_valid(transport, PF_SD_LINK_SPI) ?
           pf_spi_exchange(transport->context, tx, rx, length) : PF_SD_LINK_INVALID_ARGUMENT;
}

pf_sd_link_t pf_spi_bus(pf_spi_t *context)
{
    return (pf_sd_link_t){.context = context, .mode = PF_SD_LINK_SPI};
}
