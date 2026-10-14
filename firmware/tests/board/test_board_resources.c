#include "unity.h"
#include "board_resources.h"
#include "sd_card_protocol.h"
#include "sd_spi_protocol.h"

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
        .command_gpio = 3u,
        .clock_gpio = 2u,
        .data_gpio = { 4u, 5u, 6u, 7u },
        .pio_index = 0u,
        .clock_hz = 0u,
        .spi_index = 0u,
        .spi_clock_gpio = 2u,
        .spi_mosi_gpio = 3u,
        .spi_miso_gpio = 4u,
        .spi_chip_select_gpio = 7u,
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
        .command_gpio = PF_BOARD_PIN_UNASSIGNED,
        .clock_gpio = PF_BOARD_PIN_UNASSIGNED,
        .data_gpio = { PF_BOARD_PIN_UNASSIGNED, PF_BOARD_PIN_UNASSIGNED,
                   PF_BOARD_PIN_UNASSIGNED, PF_BOARD_PIN_UNASSIGNED },
    };

    TEST_ASSERT_TRUE(pf_board_resources_validate(&topology));
}

void test_accepts_valid_four_bit_sd_topology(void)
{
    pf_board_resources_t topology = valid_topology();

    TEST_ASSERT_TRUE(pf_board_resources_validate(&topology));
}

void test_accepts_valid_spi_sd_topology(void)
{
    pf_board_resources_t topology = valid_topology();

    TEST_ASSERT_TRUE(pf_board_resources_validate(&topology));
}

void test_rejects_invalid_spi_sd_pin_assignments(void)
{
    pf_board_resources_t topology = valid_topology();

    topology.spi_index = 2u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.spi_mosi_gpio = topology.spi_clock_gpio;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.spi_clock_gpio = topology.monitor_uart_tx_gpio;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
}

void test_rejects_spi_pins_that_do_not_match_the_controller(void)
{
    pf_board_resources_t topology = valid_topology();

    topology.spi_index = 1u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.spi_mosi_gpio = 9u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
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
    topology.monitor_uart_rx_gpio = topology.data_gpio[3];
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
    topology = valid_topology();
    topology.pio_index = 2u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.clock_hz = 399999u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.clock_gpio = 1u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
}

void test_rejects_pins_outside_sd_bus_constraints(void)
{
    pf_board_resources_t topology = valid_topology();

    topology.data_gpio[0] = 1u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.data_gpio[0] = 27u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.command_gpio = 31u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.data_gpio[1] = topology.data_gpio[0];
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.data_gpio[1] = 6u;
    topology.data_gpio[2] = 5u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.data_gpio[1] = 8u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
}

void test_rejects_command_clock_and_data_pin_collisions(void)
{
    pf_board_resources_t topology = valid_topology();

    topology.command_gpio = 2u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology = valid_topology();
    topology.command_gpio = 5u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
}

void test_validates_optional_card_detect_pin(void)
{
    pf_board_resources_t topology = valid_topology();

    topology.card_detect_enabled = true;
    topology.card_detect_gpio = 20u;
    TEST_ASSERT_TRUE(pf_board_resources_validate(&topology));
    topology.card_detect_gpio = 8u;
    TEST_ASSERT_TRUE(pf_board_resources_validate(&topology));
    topology.card_detect_gpio = topology.command_gpio;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
    topology.card_detect_gpio = 6u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
}

void test_validates_led_presence_and_sd_pin_collisions(void)
{
    pf_board_resources_t topology = valid_topology();

    topology.led_enabled = true;
    topology.led_gpio = 25u;
    topology.led_active_high = true;
    TEST_ASSERT_TRUE(pf_board_resources_validate(&topology));
    topology.led_gpio = topology.command_gpio;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));

    topology = valid_topology();
    topology.sd_card_enabled = false;
    topology.led_enabled = true;
    topology.led_gpio = 30u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&topology));
}