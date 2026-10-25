#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/sdio")

#define SD_BOARD_SOURCE "drivers/sdcard/sd_board.c"
#include SD_BOARD_SOURCE

static pf_storage_speed_t sd_speed;
static uint32_t gpio_level, gpio_inits, pull_disables;
static pf_test_sdio_pio_t fake_pio0;
PIO pio0 = &fake_pio0;
PIO pio1;

const pf_sd_board_t pf_sd_board = {.card_detect_gpio = 9u, .card_detect_active_high = false};
const pf_sdio_pio_board_t pf_sdio_pio_board = {
    .pins = {.clock_gpio = 2u, .command_gpio = 3u, .data_gpio = {4u, 5u, 6u, 7u}}, .pio_index = 0u,
};

pf_storage_speed_t pf_boot_config_storage_speed(void) { return sd_speed; }
pf_sd_link_t pf_sdio_pio_bus(pf_sdio_pio_t *host) { return (pf_sd_link_t){.mode = PF_SD_LINK_NATIVE_SD, .context = host}; }
uint32_t gpio_get(unsigned pin) { TEST_ASSERT_EQUAL_UINT(9u, pin); return gpio_level; }
void gpio_init(unsigned pin) { TEST_ASSERT_EQUAL_UINT(9u, pin); ++gpio_inits; }
void gpio_disable_pulls(unsigned pin) { TEST_ASSERT_EQUAL_UINT(9u, pin); ++pull_disables; }

void setUp(void) {}
void tearDown(void) {}

void test_board_config_binds_transport_once_and_follows_boot_speed_and_card_detect_polarity(void)
{
    pf_sd_config_t config;

    TEST_ASSERT_FALSE(pf_sd_board_config(NULL));
    TEST_ASSERT_TRUE(pf_sd_board_config(&config));
    TEST_ASSERT_EQUAL_PTR(&sd_transport, config.transport.context);
    TEST_ASSERT_EQUAL_PTR(pio0, sd_transport.config.pio);
    TEST_ASSERT_EQUAL_UINT32(2u, sd_transport.config.pins.clock_gpio);
    TEST_ASSERT_EQUAL_UINT32(25000000u, config.clock_hz);
    TEST_ASSERT_TRUE(config.native_default_speed_only);

    sd_speed = PF_STORAGE_SPEED_HIGH;
    TEST_ASSERT_TRUE(pf_sd_board_config(&config));
    TEST_ASSERT_EQUAL_UINT32(50000000u, config.clock_hz);
    TEST_ASSERT_FALSE(config.native_default_speed_only);
    TEST_ASSERT_EQUAL_UINT32(1u, gpio_inits);
    TEST_ASSERT_EQUAL_UINT32(1u, pull_disables);

    gpio_level = 0u;
    TEST_ASSERT_TRUE(config.transport.present(config.transport.presence_context));
    gpio_level = 1u;
    TEST_ASSERT_FALSE(config.transport.present(config.transport.presence_context));
}

void test_board_pins_report_sdio_order_and_card_detect(void)
{
    uint8_t pins[6];
    uint8_t detect = 0u;

    TEST_ASSERT_EQUAL(PF_SD_LINK_NATIVE_SD, pf_sd_board_pins(pins, &detect));

    const uint8_t expected[6] = {2u, 3u, 4u, 5u, 6u, 7u};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, pins, 6u);
    TEST_ASSERT_EQUAL_UINT8(9u, detect);
}
