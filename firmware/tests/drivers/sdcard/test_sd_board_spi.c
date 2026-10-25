#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/spi")

#define SD_BOARD_SOURCE "drivers/sdcard/sd_board.c"
#define SD_BOARD_TRANSPORT_SOURCE "drivers/sdcard/spi/sd_board_spi.c"
#include SD_BOARD_SOURCE
#include SD_BOARD_TRANSPORT_SOURCE

static pf_storage_speed_t sd_speed;
spi_inst_t test_spi;
bool spi_test_present;

const pf_spi_board_pins_t pf_spi_board_pins = {
    .pins = {.clock_gpio = 2u}, .spi_index = 0u, .card_detect_gpio = 10u, .card_detect_active_high = true,
};

pf_storage_speed_t pf_boot_config_storage_speed(void) { return sd_speed; }
pf_sd_link_t pf_spi_bus(pf_spi_t *context) { return (pf_sd_link_t){.mode = PF_SD_LINK_SPI, .context = context}; }

void setUp(void) {}
void tearDown(void) {}

void test_spi_clock_follows_the_speed_profile(void)
{
    pf_sd_config_t config;

    TEST_ASSERT_TRUE(pf_sd_board_config(&config));
    TEST_ASSERT_EQUAL_UINT32(400000u, sd_transport.clock_hz);
    TEST_ASSERT_EQUAL_UINT32(25000000u, config.clock_hz);
    TEST_ASSERT_FALSE(config.native_default_speed_only);
    sd_transport.clock_hz = 12000000u;
    sd_speed = PF_STORAGE_SPEED_HIGH;
    TEST_ASSERT_TRUE(pf_sd_board_config(&config));
    TEST_ASSERT_EQUAL_UINT32(50000000u, config.clock_hz);
    TEST_ASSERT_EQUAL_UINT32(12000000u, sd_transport.clock_hz);
}

void test_spi_pins_fill_four_entries_and_mark_the_rest_unused(void)
{
    uint8_t pins[6];
    uint8_t detect = 0u;

    TEST_ASSERT_EQUAL(PF_SD_LINK_SPI, pf_sd_board_pins(pins, &detect));

    const uint8_t expected[6] = {2u, 0u, 0u, 0u, UINT8_MAX, UINT8_MAX};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, pins, 6u);
    TEST_ASSERT_EQUAL_UINT8(10u, detect);
}
