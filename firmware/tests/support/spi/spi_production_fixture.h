#pragma once
#ifdef PF_SPI_PRODUCTION_TEST
/* Include the whole driver so tests can inspect private recovery state without
 * exporting test-only APIs or copying implementation functions. */
#include "./hardware/spi.h"
#include "./hardware/gpio.h"
#include "./pico/stdlib.h"
#include "sd_card_protocol.h"
static uint16_t spi_fixture_crc16(const uint8_t *data, size_t size);
#define pf_sd_card_crc16 spi_fixture_crc16
#include "../../../src/drivers/sd_card/spi/sd_spi.c"
#undef pf_sd_card_crc16
#endif
