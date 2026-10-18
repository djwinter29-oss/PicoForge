#include "unity.h"
void pf_sd_lock(void) {}
void pf_sd_unlock(void) {}
#include "board_resources.h"
#include "bus.h"
#include "config/config.h"
#include "sd_protocol.h"

static pf_board_resources_t valid_topology(void)
{
    return (pf_board_resources_t){
        .gpio_count = 30u,
        .monitor_uart_enabled = true,
        .monitor_uart_index = 0u,
        .monitor_uart_tx_gpio = 0u,
        .monitor_uart_rx_gpio = 1u,
        .monitor_uart_baud_rate = 115200u,
        .sd_card_enabled = true,
        .native_pins.command_gpio = 3u,
        .native_pins.clock_gpio = 2u,
        .native_pins.data_gpio = { 4u, 5u, 6u, 7u },
        .pio_index = 0u,
        .clock_hz = 0u,
        .spi_index = 0u,
        .spi_pins.clock_gpio = 2u,
        .spi_pins.mosi_gpio = 3u,
        .spi_pins.miso_gpio = 4u,
        .spi_pins.cs_gpio = 7u,
        .spi_clock_hz = 400000u,
    };
}

void setUp(void) {}
void tearDown(void) {}

void test_accepts_unconfigured_sd_topology(void)
{
    pf_board_resources_t topology = {
        .gpio_count = 30u,
        .monitor_uart_enabled = true,
        .monitor_uart_index = 0u,
        .monitor_uart_tx_gpio = 0u,
        .monitor_uart_rx_gpio = 1u,
        .monitor_uart_baud_rate = 115200u,
        .sd_card_enabled = false,
        .native_pins.command_gpio = PF_BOARD_PIN_UNASSIGNED,
        .native_pins.clock_gpio = PF_BOARD_PIN_UNASSIGNED,
        .native_pins.data_gpio = { PF_BOARD_PIN_UNASSIGNED, PF_BOARD_PIN_UNASSIGNED,
                   PF_BOARD_PIN_UNASSIGNED, PF_BOARD_PIN_UNASSIGNED },
    };

    TEST_ASSERT_TRUE(pf_board_resources_validate(&topology));
}

void test_accepts_valid_selected_transport_topology(void)
{
    pf_board_resources_t topology = valid_topology();

    TEST_ASSERT_TRUE(pf_board_resources_validate(&topology));
}

void test_native_clock_policy_accepts_only_default_or_25mhz(void)
{
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SDIO_PIO
    pf_board_resources_t topology = valid_topology();

    topology.clock_hz = 25000000u;
    TEST_ASSERT_TRUE(pf_board_resources_validate(&topology));
    topology.clock_hz = 50000000u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology.clock_hz = 10000000u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
#endif
}

void test_rejects_invalid_spi_sd_pin_assignments(void)
{
    pf_board_resources_t topology = valid_topology();

#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SPI
    topology.spi_index = 2u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.spi_pins.mosi_gpio = topology.spi_pins.clock_gpio;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.spi_pins.clock_gpio = topology.monitor_uart_tx_gpio;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
#else
    topology.spi_index = 2u;
    topology.spi_pins.mosi_gpio = topology.spi_pins.clock_gpio;
    TEST_ASSERT_TRUE(pf_board_resources_validate(&topology));
#endif
}

void test_spi_clock_policy_accepts_default_through_50mhz(void)
{
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SPI
    pf_board_resources_t topology = valid_topology();
    const uint32_t valid[] = {0u, 400000u, 25000000u, 50000000u};
    for (unsigned i = 0u; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        topology.spi_clock_hz = valid[i];
        TEST_ASSERT_TRUE(pf_board_resources_validate(&topology));
    }
    topology.spi_clock_hz = 399999u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology.spi_clock_hz = 50000001u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
#endif
}

void test_rejects_spi_pins_that_do_not_match_the_controller(void)
{
    pf_board_resources_t topology = valid_topology();

#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SPI
    topology.spi_index = 1u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.spi_pins.mosi_gpio = 9u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
#else
    topology.spi_index = 2u;
    topology.spi_pins.mosi_gpio = topology.spi_pins.clock_gpio;
    TEST_ASSERT_TRUE(pf_board_resources_validate(&topology));
#endif
}

void test_rejects_invalid_monitor_uart_assignment(void)
{
    pf_board_resources_t topology = valid_topology();

    topology.monitor_uart_index = 2u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.monitor_uart_tx_gpio = topology.monitor_uart_rx_gpio;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.monitor_uart_rx_gpio = topology.gpio_count;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.monitor_uart_baud_rate = 0u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
}

void test_rejects_monitor_uart_pin_conflicts(void)
{
    pf_board_resources_t topology = valid_topology();

    topology.led_enabled = true;
    topology.led_gpio = topology.monitor_uart_tx_gpio;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.monitor_uart_tx_gpio = 2u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.monitor_uart_rx_gpio = topology.native_pins.data_gpio[3];
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.card_detect_enabled = true;
    topology.card_detect_gpio = topology.monitor_uart_rx_gpio;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
}

void test_rejects_invalid_topology_pointer_pio_and_clock(void)
{
    pf_board_resources_t topology = valid_topology();

    TEST_ASSERT_FALSE(pf_board_resources_validate(NULL));
    topology.gpio_count = 0u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SDIO_PIO
    topology = valid_topology();
    topology.pio_index = 2u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.clock_hz = 399999u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.native_pins.clock_gpio = 1u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
#else
    topology = valid_topology();
    topology.pio_index = 2u;
    topology.clock_hz = 399999u;
    topology.native_pins.clock_gpio = 1u;
    TEST_ASSERT_TRUE(pf_board_resources_validate(&topology));
#endif
}

void test_rejects_pins_outside_sd_bus_constraints(void)
{
    pf_board_resources_t topology = valid_topology();

#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SDIO_PIO
    topology.native_pins.data_gpio[0] = 1u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.native_pins.data_gpio[0] = 27u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.native_pins.command_gpio = 31u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.native_pins.data_gpio[1] = topology.native_pins.data_gpio[0];
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.native_pins.data_gpio[1] = 6u;
    topology.native_pins.data_gpio[2] = 5u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.native_pins.data_gpio[1] = 8u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
#else
    topology.native_pins.data_gpio[0] = 1u;
    topology.native_pins.command_gpio = 31u;
    TEST_ASSERT_TRUE(pf_board_resources_validate(&topology));
#endif
}

void test_rejects_command_clock_and_data_pin_collisions(void)
{
    pf_board_resources_t topology = valid_topology();

#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SDIO_PIO
    topology.native_pins.command_gpio = 2u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.native_pins.command_gpio = 5u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
#else
    topology.native_pins.command_gpio = 2u;
    topology.native_pins.clock_gpio = 4u;
    TEST_ASSERT_TRUE(pf_board_resources_validate(&topology));
#endif
}

void test_validates_optional_card_detect_pin(void)
{
    pf_board_resources_t topology = valid_topology();

    topology.card_detect_enabled = true;
    topology.card_detect_gpio = 20u;
    TEST_ASSERT_TRUE(pf_board_resources_validate(&topology));
    topology.card_detect_gpio = 8u;
    TEST_ASSERT_TRUE(pf_board_resources_validate(&topology));
    topology.card_detect_gpio = topology.native_pins.command_gpio;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
#if PF_CONFIG_SD_CARD_TRANSPORT == PF_CONFIG_SD_CARD_TRANSPORT_SDIO_PIO
    topology.card_detect_gpio = 6u;
#else
    topology.card_detect_gpio = 4u;
#endif
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
}

void test_validates_led_presence_and_sd_pin_collisions(void)
{
    pf_board_resources_t topology = valid_topology();

    topology.led_enabled = true;
    topology.led_gpio = 25u;
    topology.led_active_high = true;
    TEST_ASSERT_TRUE(pf_board_resources_validate(&topology));
    topology.led_gpio = topology.native_pins.command_gpio;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));

    topology = valid_topology();
    topology.sd_card_enabled = false;
    topology.led_enabled = true;
    topology.led_gpio = 30u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
}