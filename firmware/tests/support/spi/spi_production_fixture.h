#pragma once
#ifdef PF_SPI_PRODUCTION_TEST
/* Include the whole driver so tests can inspect private recovery state without
 * exporting test-only APIs or copying implementation functions. */
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "sd_protocol.h"
static uint16_t spi_fixture_crc16(const uint8_t *data, size_t size);
#include "drivers/sdcard/sd_link.h"
#include "drivers/sdcard/spi/sd_link_spi.h"
static pf_sd_link_result_t spi_fixture_set_clock(const pf_sd_link_t *bus, uint32_t hz, uint32_t *actual);
static pf_sd_link_result_t spi_fixture_select(const pf_sd_link_t *bus, bool select);
#define pf_sd_link_set_clock spi_fixture_set_clock
#define pf_sd_link_spi_select spi_fixture_select
#define pf_sd_crc16 spi_fixture_crc16
#include "drivers/sdcard/spi/sd_backend_spi.c"
#undef pf_sd_crc16
#undef pf_sd_link_set_clock
#undef pf_sd_link_spi_select
#endif
