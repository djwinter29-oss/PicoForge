#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/serial")
#include "serial_fake.h"
#include "sd_link.h"
#include "sd_link_serial.h"
#include "spi.h"
#include <string.h>

serial_fake_t serial_fake;
spi_inst_t serial_spi[2] = {{0}, {1}};
static pf_spi_t sd;
static pf_spi_t peer;
static pf_sd_link_t peer_bus;

void setUp(void)
{
    memset(&serial_fake, 0, sizeof(serial_fake));
    sd = (pf_spi_t){ .instance = spi0, .pins.clock_gpio = 2, .pins.mosi_gpio = 3,
        .pins.miso_gpio = 0, .pins.cs_gpio = 1, .clock_hz = 25000000 };
    peer = (pf_spi_t){ .instance = spi0, .pins.clock_gpio = 2, .pins.mosi_gpio = 3,
        .pins.miso_gpio = 0, .pins.cs_gpio = 5, .clock_hz = 10000000, .timeout_us = 100000 };
    peer_bus = pf_spi_bus(&peer);
}

void tearDown(void)
{
    pf_spi_deinit(&sd);
    pf_sd_link_deinit(&peer_bus);
}

void test_spi_pin_allocation_matches_controller_roles(void)
{
    TEST_ASSERT_TRUE(pf_spi_pins_match_controller(0u, 2u, 3u, 4u));
    TEST_ASSERT_TRUE(pf_spi_pins_match_controller(0u, 6u, 7u, 0u));
    TEST_ASSERT_TRUE(pf_spi_pins_match_controller(1u, 10u, 11u, 8u));
    TEST_ASSERT_TRUE(pf_spi_pins_match_controller(0u, 34u, 35u, 36u));
    TEST_ASSERT_FALSE(pf_spi_pins_match_controller(1u, 2u, 3u, 4u));
    TEST_ASSERT_FALSE(pf_spi_pins_match_controller(0u, 2u, 3u, 5u));
    TEST_ASSERT_FALSE(pf_spi_pins_match_controller(0u, 2u, 11u, 4u));
    TEST_ASSERT_FALSE(pf_spi_pins_match_controller(2u, 2u, 3u, 4u));
}

void test_spi_devices_share_one_controller_and_restore_device_clock(void)
{
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_spi_init(&sd));
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_sd_link_init(&peer_bus));
    TEST_ASSERT_EQUAL_UINT(1, serial_fake.init);
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_spi_select(&sd, true));
    uint8_t value = 0xfc, received;
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_spi_exchange(&sd, &value, &received, 1));
    TEST_ASSERT_EQUAL_UINT32(25000000, serial_fake.hz);
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_spi_select(&sd, false));
    const uint8_t payload[] = {0xaa, 0x55};
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_sd_link_serial_select(&peer_bus, true));
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_sd_link_serial_exchange(&peer_bus, payload, NULL, sizeof(payload)));
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_sd_link_serial_select(&peer_bus, false));
    const uint8_t expected[] = {0xfc, 0xaa, 0x55};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, serial_fake.bytes, sizeof(expected));
    TEST_ASSERT_EQUAL_UINT32(10000000, serial_fake.hz);
    TEST_ASSERT_FALSE(serial_fake.selected);
    pf_sd_link_deinit(&peer_bus);
    TEST_ASSERT_EQUAL_UINT(0, serial_fake.deinit);
    pf_spi_deinit(&sd);
    TEST_ASSERT_EQUAL_UINT(1, serial_fake.deinit);
}

void test_spi_fifo_timeout_can_be_deselected_and_aborted_without_releasing_a_peer(void)
{
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_spi_init(&sd));
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_sd_link_init(&peer_bus));
    serial_fake.short_io = true;
    uint8_t output = 0xa5u;
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_sd_link_serial_select(&peer_bus, true));
    TEST_ASSERT_EQUAL(PF_SD_LINK_TIMEOUT, pf_sd_link_serial_exchange(&peer_bus, &output, NULL, 1u));
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_sd_link_serial_select(&peer_bus, false));
    TEST_ASSERT_FALSE(serial_fake.selected);
    pf_sd_link_abort(&peer_bus);
    TEST_ASSERT_FALSE(serial_fake.selected || peer.initialized);
    TEST_ASSERT_TRUE(sd.initialized);
    TEST_ASSERT_EQUAL_UINT(0, serial_fake.deinit);
}

void test_bounded_fifo_pipelines_eight_bytes_preserves_order_and_avoids_per_byte_timer_reads(void)
{
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_spi_init(&peer));
    uint8_t output[32], input[32];
    for (unsigned index = 0u; index < sizeof(output); ++index) output[index] = (uint8_t)index;
    serial_fake.delayed_reads = 9u;
    serial_fake.input = 0x91u;
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_spi_exchange(&peer, output, input, sizeof(output)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(output, serial_fake.bytes, sizeof(output));
    TEST_ASSERT_EQUAL_UINT32(8u, serial_fake.peak_pending);
    TEST_ASSERT_EQUAL_UINT32(0u, serial_fake.pending);
    TEST_ASSERT_EQUAL_UINT32(50000u, serial_fake.now);
    for (unsigned index = 0u; index < sizeof(input); ++index) TEST_ASSERT_EQUAL_HEX8(0x91u, input[index]);
}

void test_bounded_fifo_read_only_clocks_ff_and_write_only_drains_receive_fifo(void)
{
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_spi_init(&peer));
    uint8_t input[16];
    serial_fake.input = 0x5au;
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_spi_exchange(&peer, NULL, input, sizeof(input)));
    for (unsigned index = 0u; index < sizeof(input); ++index) {
        TEST_ASSERT_EQUAL_HEX8(0xffu, serial_fake.bytes[index]);
        TEST_ASSERT_EQUAL_HEX8(0x5au, input[index]);
    }
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_spi_exchange(&peer, input, NULL, sizeof(input)));
    TEST_ASSERT_EQUAL_UINT32(0u, serial_fake.pending);
    TEST_ASSERT_EQUAL_UINT32(0u, serial_fake.now);
}

void test_bounded_fifo_reports_tx_stall_without_sending_or_replaying_bytes(void)
{
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_spi_init(&peer));
    serial_fake.fifo_full = true;
    uint8_t byte = 0xa5u;
    TEST_ASSERT_EQUAL(PF_SD_LINK_TIMEOUT, pf_spi_exchange(&peer, &byte, NULL, 1u));
    TEST_ASSERT_EQUAL_UINT32(0u, serial_fake.length);
}

void test_sdk_short_bulk_io_is_reported_without_replay_or_implicit_cs_boundary(void)
{
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_spi_init(&sd));
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_spi_select(&sd, true));
    serial_fake.short_io = true;
    uint8_t output = 0xfc;
    TEST_ASSERT_EQUAL(PF_SD_LINK_IO_ERROR, pf_spi_exchange(&sd, &output, NULL, 1));
    TEST_ASSERT_EQUAL_UINT(1, serial_fake.length);
    TEST_ASSERT_TRUE(serial_fake.selected);
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_spi_select(&sd, false));
}

void test_invalid_or_unsupported_spi_transactions_never_assert_cs(void)
{
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_spi_init(&sd));
    pf_sd_link_t bus = pf_spi_bus(&sd);
    TEST_ASSERT_EQUAL(PF_SD_LINK_INVALID_ARGUMENT, pf_sd_link_serial_select(NULL, true));
    TEST_ASSERT_EQUAL(PF_SD_LINK_INVALID_ARGUMENT, pf_sd_link_serial_exchange(&bus, NULL, NULL, 1u));
    bus.mode = PF_SD_LINK_NATIVE_SD;
    TEST_ASSERT_EQUAL(PF_SD_LINK_INVALID_ARGUMENT, pf_sd_link_serial_select(&bus, true));
    TEST_ASSERT_EQUAL_UINT(0, serial_fake.cs_low);
}

void test_static_spi_lifecycle_rejects_invalid_handles_without_hardware_access(void)
{
    pf_sd_link_t bus = pf_spi_bus(&sd);
    uint32_t actual = 17u;
    TEST_ASSERT_EQUAL(PF_SD_LINK_INVALID_ARGUMENT, pf_sd_link_init(NULL));
    TEST_ASSERT_EQUAL(PF_SD_LINK_INVALID_ARGUMENT, pf_sd_link_set_clock(NULL, 25000000u, &actual));
    TEST_ASSERT_EQUAL(PF_SD_LINK_INVALID_ARGUMENT, pf_sd_link_set_clock(&bus, 0u, &actual));
    TEST_ASSERT_EQUAL(PF_SD_LINK_INVALID_ARGUMENT, pf_sd_link_set_clock(&bus, 25000000u, NULL));
    bus.mode = PF_SD_LINK_NATIVE_SD;
    TEST_ASSERT_EQUAL(PF_SD_LINK_INVALID_ARGUMENT, pf_sd_link_init(&bus));
    TEST_ASSERT_EQUAL(PF_SD_LINK_INVALID_ARGUMENT, pf_sd_link_set_clock(&bus, 25000000u, &actual));
    TEST_ASSERT_EQUAL_UINT32(17u, actual);
    pf_sd_link_abort(NULL);
    pf_sd_link_deinit(&bus);
    TEST_ASSERT_EQUAL_UINT(0u, serial_fake.init);
    TEST_ASSERT_EQUAL_UINT(0u, serial_fake.cs_low);
}

void test_selected_device_prevents_another_device_from_clocking_the_bus(void)
{
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_spi_init(&sd));
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_sd_link_init(&peer_bus));
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_spi_select(&sd, true));
    pf_spi_t another = sd;
    another.initialized = false;
    another.pins.cs_gpio = 8;
    unsigned cs_high = serial_fake.cs_high;
    TEST_ASSERT_EQUAL(PF_SD_LINK_NOT_READY, pf_spi_init(&another));
    TEST_ASSERT_EQUAL_UINT(cs_high, serial_fake.cs_high);
    uint8_t output = 0xa5u;
    TEST_ASSERT_EQUAL(PF_SD_LINK_NOT_READY, pf_sd_link_serial_select(&peer_bus, true));
    TEST_ASSERT_EQUAL(PF_SD_LINK_NOT_READY, pf_sd_link_serial_exchange(&peer_bus, &output, NULL, 1u));
    TEST_ASSERT_EQUAL_UINT(0, serial_fake.length);
    TEST_ASSERT_TRUE(sd.initialized);
    TEST_ASSERT_TRUE(peer.initialized);
}

void test_generic_spi_dispatch_clocks_dummy_bytes_and_receives_payload(void)
{
    pf_sd_link_t bus = pf_spi_bus(&sd);
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_sd_link_init(&bus));
    serial_fake.input = 0x91;
    uint8_t input[2];
    uint8_t output = 0x0b;
    const uint8_t dummy[] = {0xffu, 0xffu};
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_sd_link_serial_select(&bus, true));
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_sd_link_serial_exchange(&bus, &output, NULL, 1u));
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_sd_link_serial_exchange(&bus, dummy, NULL, sizeof(dummy)));
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_sd_link_serial_exchange(&bus, NULL, input, sizeof(input)));
    TEST_ASSERT_EQUAL(PF_SD_LINK_OK, pf_sd_link_serial_select(&bus, false));
    const uint8_t expected[] = {0x0b, 0xff, 0xff, 0xff, 0xff};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, serial_fake.bytes, sizeof(expected));
    TEST_ASSERT_EQUAL_HEX8(0x91, input[0]);
    TEST_ASSERT_FALSE(serial_fake.selected);
}
