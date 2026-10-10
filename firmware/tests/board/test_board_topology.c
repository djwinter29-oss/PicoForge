#include "unity.h"
#include "board_topology.h"

static picoforge_board_topology_t valid_topology(void)
{
    return (picoforge_board_topology_t){
        .gpio_count = 30u,
        .sdio_enabled = true,
        .command_gpio = 10u,
        .data0_gpio = 4u,
        .pio_index = 0u,
        .clock_hz = 0u,
    };
}

void setUp(void) {}
void tearDown(void) {}

void test_accepts_unconfigured_sd_topology(void)
{
    picoforge_board_topology_t topology = {
        .gpio_count = 30u,
        .sdio_enabled = false,
        .command_gpio = PICOFORGE_BOARD_PIN_UNASSIGNED,
        .data0_gpio = PICOFORGE_BOARD_PIN_UNASSIGNED,
    };

    TEST_ASSERT_TRUE(picoforge_board_topology_validate(&topology));
}

void test_accepts_valid_four_bit_sd_topology(void)
{
    picoforge_board_topology_t topology = valid_topology();

    TEST_ASSERT_TRUE(picoforge_board_topology_validate(&topology));
}

void test_rejects_invalid_topology_pointer_pio_and_clock(void)
{
    picoforge_board_topology_t topology = valid_topology();

    TEST_ASSERT_FALSE(picoforge_board_topology_validate(NULL));
    topology.gpio_count = 0u;
    TEST_ASSERT_FALSE(picoforge_board_topology_validate(&topology));
    topology = valid_topology();
    topology.pio_index = 2u;
    TEST_ASSERT_FALSE(picoforge_board_topology_validate(&topology));
    topology = valid_topology();
    topology.clock_hz = 399999u;
    TEST_ASSERT_FALSE(picoforge_board_topology_validate(&topology));
}

void test_rejects_pins_outside_sd_bus_constraints(void)
{
    picoforge_board_topology_t topology = valid_topology();

    topology.data0_gpio = 1u;
    TEST_ASSERT_FALSE(picoforge_board_topology_validate(&topology));
    topology = valid_topology();
    topology.data0_gpio = 27u;
    TEST_ASSERT_FALSE(picoforge_board_topology_validate(&topology));
    topology = valid_topology();
    topology.command_gpio = 31u;
    TEST_ASSERT_FALSE(picoforge_board_topology_validate(&topology));
}

void test_rejects_command_clock_and_data_pin_collisions(void)
{
    picoforge_board_topology_t topology = valid_topology();

    topology.command_gpio = 2u;
    TEST_ASSERT_FALSE(picoforge_board_topology_validate(&topology));
    topology = valid_topology();
    topology.command_gpio = 5u;
    TEST_ASSERT_FALSE(picoforge_board_topology_validate(&topology));
}

void test_validates_optional_card_detect_pin(void)
{
    picoforge_board_topology_t topology = valid_topology();

    topology.card_detect_enabled = true;
    topology.card_detect_gpio = 20u;
    TEST_ASSERT_TRUE(picoforge_board_topology_validate(&topology));
    topology.card_detect_gpio = 0u;
    TEST_ASSERT_TRUE(picoforge_board_topology_validate(&topology));
        topology.card_detect_gpio = 0u;
        TEST_ASSERT_TRUE(picoforge_board_topology_validate(&topology));
    topology.card_detect_gpio = topology.command_gpio;
    TEST_ASSERT_FALSE(picoforge_board_topology_validate(&topology));
    topology.card_detect_gpio = 6u;
    TEST_ASSERT_FALSE(picoforge_board_topology_validate(&topology));
}

void test_validates_led_presence_and_sd_pin_collisions(void)
{
    picoforge_board_topology_t topology = valid_topology();

    topology.led_enabled = true;
    topology.led_gpio = 25u;
    topology.led_active_high = true;
    TEST_ASSERT_TRUE(picoforge_board_topology_validate(&topology));
    topology.led_gpio = topology.command_gpio;
    TEST_ASSERT_FALSE(picoforge_board_topology_validate(&topology));

    topology = valid_topology();
    topology.sdio_enabled = false;
    topology.led_enabled = true;
    topology.led_gpio = 30u;
    TEST_ASSERT_FALSE(picoforge_board_topology_validate(&topology));
}