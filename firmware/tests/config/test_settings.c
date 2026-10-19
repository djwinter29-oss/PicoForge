#include "unity.h"

#include <string.h>

#include "config/settings.h"

void setUp(void) {}
void tearDown(void) {}

static void valid_record(uint8_t record[PF_SETTINGS_RECORD_SIZE])
{
    pf_settings_t settings = {.system_clock_mhz = 200u, .sdio_bus_mhz = 50u, .spi_bus_mhz = 25u};
    TEST_ASSERT_TRUE(pf_settings_encode(&settings, record));
}

static void reseal(uint8_t record[PF_SETTINGS_RECORD_SIZE])
{
    uint32_t crc = pf_settings_crc32(record, PF_SETTINGS_RECORD_SIZE - 4u);
    for (unsigned i = 0u; i < 4u; ++i) {
        record[PF_SETTINGS_RECORD_SIZE - 4u + i] = (uint8_t)(crc >> (8u * i));
    }
}

static void assert_defaults(const uint8_t *record, size_t size)
{
    pf_settings_t settings = {.system_clock_mhz = 250u, .sdio_bus_mhz = 50u, .spi_bus_mhz = 50u};
    TEST_ASSERT_FALSE(pf_settings_decode(record, size, &settings));
    TEST_ASSERT_EQUAL_UINT16(0u, settings.system_clock_mhz);
    TEST_ASSERT_EQUAL_UINT8(0u, settings.sdio_bus_mhz);
    TEST_ASSERT_EQUAL_UINT8(0u, settings.spi_bus_mhz);
}

void test_crc32_matches_the_standard_check_value(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xcbf43926u, pf_settings_crc32((const uint8_t *)"123456789", 9u));
}

void test_defaults_are_complete_and_valid(void)
{
    pf_settings_t settings = {1u, 2u, 3u};
    pf_settings_defaults(&settings);
    TEST_ASSERT_EQUAL_UINT16(0u, settings.system_clock_mhz);
    TEST_ASSERT_EQUAL_UINT8(0u, settings.sdio_bus_mhz);
    TEST_ASSERT_EQUAL_UINT8(0u, settings.spi_bus_mhz);
    TEST_ASSERT_TRUE(pf_settings_valid(&settings));
}

void test_round_trip_and_fixed_layout(void)
{
    uint8_t record[PF_SETTINGS_RECORD_SIZE];
    pf_settings_t settings;
    valid_record(record);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){'P', 'F', 'C', 'F', 1, 0, 16, 0, 200, 0, 50, 25}), record, 12);
    TEST_ASSERT_TRUE(pf_settings_decode(record, sizeof(record), &settings));
    TEST_ASSERT_EQUAL_UINT16(200u, settings.system_clock_mhz);
    TEST_ASSERT_EQUAL_UINT8(50u, settings.sdio_bus_mhz);
    TEST_ASSERT_EQUAL_UINT8(25u, settings.spi_bus_mhz);
}

void test_every_supported_value_round_trips_and_others_are_rejected(void)
{
    const uint16_t clocks[] = {0u, 150u, 200u, 250u};
    const uint8_t buses[] = {0u, 25u, 50u};
    for (unsigned c = 0u; c < 4u; ++c) {
        for (unsigned b = 0u; b < 3u; ++b) {
            uint8_t record[PF_SETTINGS_RECORD_SIZE];
            pf_settings_t in = {clocks[c], buses[b], buses[(b + 1u) % 3u]}, out;
            TEST_ASSERT_TRUE(pf_settings_encode(&in, record));
            TEST_ASSERT_TRUE(pf_settings_decode(record, sizeof(record), &out));
            TEST_ASSERT_EQUAL_MEMORY(&in, &out, sizeof(in));
        }
    }
    uint8_t record[PF_SETTINGS_RECORD_SIZE];
    const pf_settings_t bad[] = {{100u, 0u, 0u}, {125u, 0u, 0u}, {251u, 0u, 0u}, {0u, 10u, 0u},
                                 {0u, 100u, 0u}, {0u, 0u, 10u}, {0u, 0u, 75u}};
    for (unsigned i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        TEST_ASSERT_FALSE(pf_settings_valid(&bad[i]));
        TEST_ASSERT_FALSE(pf_settings_encode(&bad[i], record));
    }
}

void test_missing_short_and_erased_data_return_defaults(void)
{
    uint8_t record[PF_SETTINGS_RECORD_SIZE];
    uint8_t erased[PF_SETTINGS_RECORD_SIZE];
    pf_settings_t settings = {150u, 25u, 25u};
    memset(erased, 0xff, sizeof(erased));
    valid_record(record);
    assert_defaults(NULL, sizeof(record));
    assert_defaults(record, PF_SETTINGS_RECORD_SIZE - 1u);
    assert_defaults(erased, sizeof(erased));
    TEST_ASSERT_FALSE(pf_settings_decode(record, sizeof(record), NULL));
    TEST_ASSERT_TRUE(pf_settings_decode(record, sizeof(record), &settings));
}

void test_crc_covers_every_byte_before_it(void)
{
    uint8_t record[PF_SETTINGS_RECORD_SIZE];
    for (unsigned byte = 0u; byte < PF_SETTINGS_RECORD_SIZE; ++byte) {
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            valid_record(record);
            record[byte] ^= (uint8_t)(1u << bit);
            assert_defaults(record, sizeof(record));
        }
    }
}

void test_unknown_version_size_magic_are_rejected_even_with_valid_crc(void)
{
    uint8_t record[PF_SETTINGS_RECORD_SIZE];
    const unsigned offsets[] = {0u, 4u, 6u};
    for (unsigned i = 0u; i < 3u; ++i) {
        valid_record(record);
        record[offsets[i]] ^= 0x01u;
        reseal(record);
        assert_defaults(record, sizeof(record));
    }
}

void test_out_of_range_fields_are_rejected_even_with_valid_crc(void)
{
    const struct { unsigned offset; uint8_t value; } cases[] = {{8u, 100u}, {9u, 1u}, {10u, 40u}, {11u, 75u}};
    uint8_t record[PF_SETTINGS_RECORD_SIZE];
    for (unsigned i = 0u; i < 4u; ++i) {
        valid_record(record);
        record[cases[i].offset] = cases[i].value;
        reseal(record);
        assert_defaults(record, sizeof(record));
    }
}
