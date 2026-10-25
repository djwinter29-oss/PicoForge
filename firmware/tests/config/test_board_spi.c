#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/sdio")

#define SPI_ASSIGNMENT_SOURCE "config/board_spi.c"
#include SPI_ASSIGNMENT_SOURCE

#ifdef PF_SDIO_PIO_PINS_H
#error "SPI board configuration must not include SDIO assignments"
#endif

void setUp(void) {}
void tearDown(void) {}

void test_board_config_defines_only_the_spi_socket_assignment(void)
{
    TEST_ASSERT_EQUAL_UINT(7u, sizeof(pf_spi_board_pins));
    TEST_ASSERT_EQUAL_UINT(1u, sizeof(pf_spi_board_pins.spi_index));
    TEST_ASSERT_EQUAL_UINT8(0u, pf_spi_board_pins.spi_index);
    TEST_ASSERT_EQUAL_UINT8(2u, pf_spi_board_pins.pins.clock_gpio);
    TEST_ASSERT_EQUAL_UINT8(3u, pf_spi_board_pins.pins.mosi_gpio);
    TEST_ASSERT_EQUAL_UINT8(4u, pf_spi_board_pins.pins.miso_gpio);
    TEST_ASSERT_EQUAL_UINT8(7u, pf_spi_board_pins.pins.cs_gpio);
    TEST_ASSERT_EQUAL_UINT8(8u, pf_spi_board_pins.card_detect_gpio);
    TEST_ASSERT_TRUE(pf_spi_board_pins.card_detect_active_high);
}
