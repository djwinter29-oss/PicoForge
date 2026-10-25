#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/serial")
#include "serial_fake.h"
#include "bus.h"
#include "serial.h"
#include "spi.h"
#include <string.h>

serial_fake_t serial_fake;
spi_inst_t serial_spi[2] = {{0}, {1}};
static pf_spi_t sd;
static pf_spi_t peer;
static pf_storage_bus_t peer_bus;

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
    pf_storage_bus_deinit(&peer_bus);
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
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_spi_init(&sd));
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_bus_init(&peer_bus));
    TEST_ASSERT_EQUAL_UINT(1, serial_fake.init);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_spi_select(&sd, true));
    uint8_t value = 0xfc, received;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_spi_exchange(&sd, &value, &received, 1));
    TEST_ASSERT_EQUAL_UINT32(25000000, serial_fake.hz);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_spi_select(&sd, false));
    const uint8_t payload[] = {0xaa, 0x55};
    pf_storage_serial_phase_t phase = {.tx = payload, .bit_count = sizeof(payload) * 8u, .lanes = 1u};
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_serial_transfer(&peer_bus, &phase, 1u));
    const uint8_t expected[] = {0xfc, 0xaa, 0x55};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, serial_fake.bytes, sizeof(expected));
    TEST_ASSERT_EQUAL_UINT32(10000000, serial_fake.hz);
    TEST_ASSERT_FALSE(serial_fake.selected);
    pf_storage_bus_deinit(&peer_bus);
    TEST_ASSERT_EQUAL_UINT(0, serial_fake.deinit);
    pf_spi_deinit(&sd);
    TEST_ASSERT_EQUAL_UINT(1, serial_fake.deinit);
}

void test_spi_fifo_timeout_releases_cs_and_only_its_controller_reference(void)
{
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_spi_init(&sd));
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_bus_init(&peer_bus));
    serial_fake.short_io = true;
    uint8_t output = 0xa5u;
    pf_storage_serial_phase_t phase = {.tx = &output, .bit_count = 8u, .lanes = 1u};
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_TIMEOUT, pf_storage_serial_transfer(&peer_bus, &phase, 1u));
    TEST_ASSERT_FALSE(serial_fake.selected);
    pf_storage_bus_abort(&peer_bus);
    TEST_ASSERT_FALSE(serial_fake.selected || peer.initialized);
    TEST_ASSERT_TRUE(sd.initialized);
    TEST_ASSERT_EQUAL_UINT(0, serial_fake.deinit);
}

void test_bounded_fifo_pipelines_eight_bytes_preserves_order_and_avoids_per_byte_timer_reads(void)
{
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_spi_init(&peer));
    uint8_t output[32], input[32];
    for (unsigned index = 0u; index < sizeof(output); ++index) output[index] = (uint8_t)index;
    serial_fake.delayed_reads = 9u;
    serial_fake.input = 0x91u;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_spi_exchange(&peer, output, input, sizeof(output)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(output, serial_fake.bytes, sizeof(output));
    TEST_ASSERT_EQUAL_UINT32(8u, serial_fake.peak_pending);
    TEST_ASSERT_EQUAL_UINT32(0u, serial_fake.pending);
    TEST_ASSERT_EQUAL_UINT32(50000u, serial_fake.now);
    for (unsigned index = 0u; index < sizeof(input); ++index) TEST_ASSERT_EQUAL_HEX8(0x91u, input[index]);
}

void test_bounded_fifo_read_only_clocks_ff_and_write_only_drains_receive_fifo(void)
{
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_spi_init(&peer));
    uint8_t input[16];
    serial_fake.input = 0x5au;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_spi_exchange(&peer, NULL, input, sizeof(input)));
    for (unsigned index = 0u; index < sizeof(input); ++index) {
        TEST_ASSERT_EQUAL_HEX8(0xffu, serial_fake.bytes[index]);
        TEST_ASSERT_EQUAL_HEX8(0x5au, input[index]);
    }
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_spi_exchange(&peer, input, NULL, sizeof(input)));
    TEST_ASSERT_EQUAL_UINT32(0u, serial_fake.pending);
    TEST_ASSERT_EQUAL_UINT32(0u, serial_fake.now);
}

void test_bounded_fifo_reports_tx_stall_without_sending_or_replaying_bytes(void)
{
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_spi_init(&peer));
    serial_fake.fifo_full = true;
    uint8_t byte = 0xa5u;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_TIMEOUT, pf_spi_exchange(&peer, &byte, NULL, 1u));
    TEST_ASSERT_EQUAL_UINT32(0u, serial_fake.length);
}

void test_sdk_short_bulk_io_is_reported_without_replay_or_implicit_cs_boundary(void)
{
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_spi_init(&sd));
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_spi_select(&sd, true));
    serial_fake.short_io = true;
    uint8_t output = 0xfc;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_IO_ERROR, pf_spi_exchange(&sd, &output, NULL, 1));
    TEST_ASSERT_EQUAL_UINT(1, serial_fake.length);
    TEST_ASSERT_TRUE(serial_fake.selected);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_spi_select(&sd, false));
}

void test_invalid_or_unsupported_spi_transactions_never_assert_cs(void)
{
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_spi_init(&sd));
    uint8_t output = 6u;
    pf_storage_serial_phase_t phases[] = {
        {.tx = &output, .bit_count = 8u, .lanes = 1u}, {.bit_count = 8u, .lanes = 4u},
    };
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_UNSUPPORTED, pf_spi_transfer(&sd, phases, 2u));
    phases[1].lanes = 1u;
    phases[1].bit_count = 1u;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_UNSUPPORTED, pf_spi_transfer(&sd, phases, 2u));
    TEST_ASSERT_EQUAL_UINT(0, serial_fake.cs_low);
}

void test_selected_device_prevents_another_device_from_clocking_the_bus(void)
{
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_spi_init(&sd));
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_bus_init(&peer_bus));
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_spi_select(&sd, true));
    pf_spi_t another = sd;
    another.initialized = false;
    another.pins.cs_gpio = 8;
    unsigned cs_high = serial_fake.cs_high;
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_NOT_READY, pf_spi_init(&another));
    TEST_ASSERT_EQUAL_UINT(cs_high, serial_fake.cs_high);
    uint8_t output = 0xa5u;
    pf_storage_serial_phase_t phase = {.tx = &output, .bit_count = 8u, .lanes = 1u};
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_NOT_READY, pf_storage_serial_transfer(&peer_bus, &phase, 1u));
    TEST_ASSERT_EQUAL_UINT(0, serial_fake.length);
    TEST_ASSERT_TRUE(sd.initialized);
    TEST_ASSERT_TRUE(peer.initialized);
}

void test_generic_spi_dispatch_clocks_dummy_bytes_and_receives_payload(void)
{
    pf_storage_bus_t bus = pf_spi_bus(&sd);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_bus_init(&bus));
    serial_fake.input = 0x91;
    uint8_t input[2];
    uint8_t output = 0x0b;
    pf_storage_serial_phase_t phases[] = {
        {.tx = &output, .bit_count = 8u, .lanes = 1u},
        {.bit_count = 16u, .lanes = 1u},
        {.rx = input, .bit_count = sizeof(input) * 8u, .lanes = 1u},
    };
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_OK, pf_storage_serial_transfer(&bus, phases, 3u));
    const uint8_t expected[] = {0x0b, 0xff, 0xff, 0xff, 0xff};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, serial_fake.bytes, sizeof(expected));
    TEST_ASSERT_EQUAL_HEX8(0x91, input[0]);
    TEST_ASSERT_FALSE(serial_fake.selected);
}
