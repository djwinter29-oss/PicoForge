#include "unity.h"
#include "board_resources.h"
#include "bus.h"
#include <string.h>

static pf_board_resources_t profile(void)
{
    return (pf_board_resources_t){
        .gpio_count = 30u,
        .monitor_uart_enabled = true, .monitor_uart_index = 0u,
        .monitor_uart_tx_gpio = 0u, .monitor_uart_rx_gpio = 1u, .monitor_uart_baud_rate = 115200u,
        .led_enabled = true, .led_gpio = 25u,
        .spi_index = 0u, .spi_pins = {.clock_gpio = 2u, .mosi_gpio = 3u, .miso_gpio = 4u, .cs_gpio = 7u},
        .qspi_pins = {.clock_gpio = 2u, .cs_gpio = 7u, .data_gpio = {3u, 4u, 5u, 6u}},
        .serial_wp_gpio = 5u, .serial_hold_gpio = 6u,
        .serial_clock_hz = 1000000u, .serial_timeout_us = 100000u,
    };
}

void setUp(void) {}
void tearDown(void) {}

void test_nor_validation_ignores_unused_sd_topology_but_checks_common_resources(void)
{
    pf_board_resources_t value = profile();
    value.sd_card_enabled = true;
    memset(&value.native_pins, 0xff, sizeof(value.native_pins));
    value.card_detect_enabled = true;
    value.card_detect_gpio = PF_BOARD_PIN_UNASSIGNED;
    TEST_ASSERT_TRUE(pf_board_resources_validate(&value));
    TEST_ASSERT_TRUE(pf_board_resources_validate_serial(&value, true));
    value.monitor_uart_baud_rate = 0u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&value));
    TEST_ASSERT_FALSE(pf_board_resources_validate_serial(&value, true));
    TEST_ASSERT_FALSE(pf_board_resources_validate(NULL));
}

void test_every_serial_signal_rejects_uart_led_duplicates_and_out_of_range_pins(void)
{
    for (unsigned quad = 0u; quad < 2u; ++quad) {
        for (unsigned signal = 0u; signal < 6u; ++signal) {
            pf_board_resources_t value = profile();
            uint32_t *spi[] = {&value.spi_pins.clock_gpio, &value.spi_pins.cs_gpio,
                &value.spi_pins.mosi_gpio, &value.spi_pins.miso_gpio,
                &value.serial_wp_gpio, &value.serial_hold_gpio};
            uint32_t *qspi[] = {&value.qspi_pins.clock_gpio, &value.qspi_pins.cs_gpio,
                &value.qspi_pins.data_gpio[0], &value.qspi_pins.data_gpio[1],
                &value.qspi_pins.data_gpio[2], &value.qspi_pins.data_gpio[3]};
            uint32_t original = *(quad ? qspi[signal] : spi[signal]);
            value.monitor_uart_tx_gpio = original;
            TEST_ASSERT_FALSE(pf_board_resources_validate_serial(&value, quad != 0u));
            value.monitor_uart_tx_gpio = 0u;
            value.led_gpio = original;
            TEST_ASSERT_FALSE(pf_board_resources_validate_serial(&value, quad != 0u));
            value.led_gpio = 25u;
            *(quad ? qspi[signal] : spi[signal]) = signal == 1u ? 2u : 7u;
            TEST_ASSERT_FALSE(pf_board_resources_validate_serial(&value, quad != 0u));
            *(quad ? qspi[signal] : spi[signal]) = value.gpio_count;
            TEST_ASSERT_FALSE(pf_board_resources_validate_serial(&value, quad != 0u));
        }
    }
}

void test_serial_validation_checks_selected_controller_timing_and_quad_lane_order(void)
{
    pf_board_resources_t value = profile();
    value.spi_index = 1u;
    TEST_ASSERT_FALSE(pf_board_resources_validate_serial(&value, false));
    TEST_ASSERT_TRUE(pf_board_resources_validate_serial(&value, true));
    value = profile();
    value.serial_timeout_us = 0u;
    TEST_ASSERT_FALSE(pf_board_resources_validate_serial(&value, false));
    value = profile();
    value.serial_clock_hz = 0u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&value));
    value.serial_clock_hz = 10000001u;
    TEST_ASSERT_FALSE(pf_board_resources_validate(&value));
    value = profile();
    value.pio_index = 2u;
    TEST_ASSERT_FALSE(pf_board_resources_validate_serial(&value, true));
    value = profile();
    value.qspi_pins.data_gpio[1] = 8u;
    TEST_ASSERT_FALSE(pf_board_resources_validate_serial(&value, true));
}

void test_sdk_gpio_range_is_respected_without_applying_pio_limits_to_spi_or_chip_select(void)
{
    pf_board_resources_t value = profile();
    value.gpio_count = 48u;
    value.spi_pins = (pf_storage_spi_pins_t){.clock_gpio = 34u, .mosi_gpio = 35u,
        .miso_gpio = 36u, .cs_gpio = 47u};
    value.serial_wp_gpio = 45u;
    value.serial_hold_gpio = 46u;
    TEST_ASSERT_TRUE(pf_board_resources_validate_serial(&value, false));
    value.qspi_pins.cs_gpio = 47u;
    TEST_ASSERT_TRUE(pf_board_resources_validate_serial(&value, true));
    value.qspi_pins.clock_gpio = 34u;
    TEST_ASSERT_FALSE(pf_board_resources_validate_serial(&value, true));
    value.qspi_pins.clock_gpio = 2u;
    for (unsigned lane = 0u; lane < 4u; ++lane) {
        value.qspi_pins.data_gpio[lane] = 32u + lane;
    }
    TEST_ASSERT_FALSE(pf_board_resources_validate_serial(&value, true));
}
