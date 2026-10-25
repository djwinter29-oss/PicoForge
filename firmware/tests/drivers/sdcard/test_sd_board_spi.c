#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/spi")

#define PF_CONFIG_SDCARD_TRANSPORT 1
#define SD_BOARD_SOURCE "drivers/sdcard/sd_board.c"
#include SD_BOARD_SOURCE

static pf_sd_speed_t sd_speed;
spi_inst_t test_spi;
bool spi_test_present;

const pf_sd_board_t pf_sd_board = {.clock_hz = 50000000u};
const pf_spi_board_t pf_spi_board = {.pins = {.clock_gpio = 2u}, .spi_index = 0u};

pf_sd_speed_t pf_boot_config_sd_speed(void) { return sd_speed; }
pf_sd_link_t pf_spi_bus(pf_spi_t *context) { return (pf_sd_link_t){.mode = PF_SD_LINK_SPI, .context = context}; }

void setUp(void) {}
void tearDown(void) {}

void test_spi_clock_follows_the_speed_profile(void)
{
    pf_sd_config_t config;

    TEST_ASSERT_TRUE(pf_sd_board_config(&config));
    TEST_ASSERT_EQUAL_UINT32(400000u, sd_transport.clock_hz);
    TEST_ASSERT_EQUAL_UINT32(25000000u, config.clock_hz);
    sd_speed = PF_SD_SPEED_HIGH;
    TEST_ASSERT_TRUE(pf_sd_board_config(&config));
    TEST_ASSERT_EQUAL_UINT32(50000000u, config.clock_hz);
}
