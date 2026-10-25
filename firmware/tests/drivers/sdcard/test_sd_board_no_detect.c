#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/sdio")

#define SD_BOARD_SOURCE "drivers/sdcard/sd_board.c"
#include SD_BOARD_SOURCE

pf_storage_speed_t pf_boot_config_storage_speed(void) { return PF_STORAGE_SPEED_DEFAULT; }
pf_sd_link_t pf_sd_board_transport(bool bind)
{
    (void)bind;
    return (pf_sd_link_t){.mode = PF_SD_LINK_NATIVE_SD};
}
pf_sd_link_mode_t pf_sd_board_transport_pins(uint8_t bus_gpios[6])
{
    (void)bus_gpios;
    return PF_SD_LINK_NATIVE_SD;
}
uint8_t pf_sd_board_transport_detect(bool *active_high)
{
    *active_high = false;
    return UINT8_MAX;
}
uint32_t gpio_get(unsigned pin)
{
    (void)pin;
    TEST_FAIL_MESSAGE("No detect GPIO may be read");
    return 0u;
}
void gpio_init(unsigned pin)
{
    (void)pin;
    TEST_FAIL_MESSAGE("No detect GPIO may be initialized");
}
void gpio_disable_pulls(unsigned pin)
{
    (void)pin;
    TEST_FAIL_MESSAGE("No detect GPIO pulls may be changed");
}

void setUp(void) { sd_board_bound = false; }
void tearDown(void) {}

void test_no_detect_pin_assumes_presence_without_accessing_gpio_255(void)
{
    pf_sd_config_t config;
    TEST_ASSERT_TRUE(pf_sd_board_config(&config));
    TEST_ASSERT_TRUE(config.transport.present(config.transport.presence_context));
    uint8_t bus[6], detect;
    TEST_ASSERT_EQUAL(PF_SD_LINK_NATIVE_SD, pf_sd_board_pins(bus, &detect));
    TEST_ASSERT_EQUAL_UINT8(255u, detect);
}
