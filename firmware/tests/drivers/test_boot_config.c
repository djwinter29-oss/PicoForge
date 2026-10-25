#include "unity.h"

#include <string.h>

TEST_INCLUDE_PATH("firmware/tests/support/config")

#include "FreeRTOS.h"
#include "task.h"
#include "drivers/boot_config.h"
#include "platform/boot_config_store.h"

uint8_t pf_test_flash[PICO_FLASH_SIZE_BYTES];
int pf_test_safe_execute_result = PICO_OK;
int pf_test_in_isr;
static BaseType_t scheduler_state = taskSCHEDULER_RUNNING;
static UBaseType_t task_count = 5u;

BaseType_t xPortIsInsideInterrupt(void) { return pf_test_in_isr; }
BaseType_t xTaskGetSchedulerState(void) { return scheduler_state; }
UBaseType_t uxTaskGetNumberOfTasks(void) { return task_count; }
void vTaskDelay(TickType_t ticks) { (void)ticks; task_count = 5u; }

void flash_range_erase(uint32_t offset, size_t count)
{
    memset(pf_test_flash + offset, 0xff, count);
}

void flash_range_program(uint32_t offset, const uint8_t *data, size_t count)
{
    memcpy(pf_test_flash + offset, data, count);
}

int flash_safe_execute(void (*func)(void *), void *param, uint32_t timeout_ms)
{
    (void)timeout_ms;
    task_count = 6u;
    if (pf_test_safe_execute_result == PICO_OK) {
        func(param);
    }
    return pf_test_safe_execute_result;
}

void setUp(void)
{
    memset(pf_test_flash, 0xff, sizeof(pf_test_flash));
    pf_test_safe_execute_result = PICO_OK;
    pf_test_in_isr = 0;
    scheduler_state = taskSCHEDULER_NOT_STARTED;
    task_count = 5u;
    pf_boot_config_t defaults;
    TEST_ASSERT_FALSE(pf_boot_config_load_early(&defaults));
    scheduler_state = taskSCHEDULER_RUNNING;
}
void tearDown(void) {}

void test_early_load_activates_the_complete_saved_configuration(void)
{
    const pf_boot_config_t saved = {.system_clock_mhz = 200u, .sdio_bus_mhz = 50u, .spi_bus_mhz = 25u};
    pf_boot_config_t loaded;
    TEST_ASSERT_TRUE(pf_boot_config_save(&saved));
    scheduler_state = taskSCHEDULER_NOT_STARTED;
    TEST_ASSERT_TRUE(pf_boot_config_load_early(&loaded));
    scheduler_state = taskSCHEDULER_RUNNING;
    TEST_ASSERT_EQUAL_MEMORY(&saved, &loaded, sizeof(saved));
    TEST_ASSERT_EQUAL_MEMORY(&saved, pf_boot_config_active(), sizeof(saved));
}

void test_early_load_activates_defaults_when_storage_is_empty(void)
{
    pf_boot_config_t loaded = {.system_clock_mhz = 250u, .sdio_bus_mhz = 50u, .spi_bus_mhz = 50u};
    scheduler_state = taskSCHEDULER_NOT_STARTED;
    TEST_ASSERT_FALSE(pf_boot_config_load_early(&loaded));
    scheduler_state = taskSCHEDULER_RUNNING;
    TEST_ASSERT_EQUAL_UINT16(0u, pf_boot_config_active()->system_clock_mhz);
    TEST_ASSERT_EQUAL_UINT8(0u, pf_boot_config_active()->sdio_bus_mhz);
    TEST_ASSERT_EQUAL_UINT8(0u, pf_boot_config_active()->spi_bus_mhz);
}

void test_boot_config_save_and_load_use_the_platform_store(void)
{
    pf_boot_config_t saved = {.system_clock_mhz = 200u, .sdio_bus_mhz = 50u, .spi_bus_mhz = 25u};
    pf_boot_config_t loaded = {.system_clock_mhz = 150u};
    TEST_ASSERT_TRUE(pf_boot_config_save(&saved));
    TEST_ASSERT_TRUE(pf_boot_config_load(&loaded));
    TEST_ASSERT_EQUAL_MEMORY(&saved, &loaded, sizeof(saved));
}

void test_boot_config_load_returns_defaults_when_platform_store_is_unavailable(void)
{
    pf_boot_config_t loaded = {.system_clock_mhz = 250u, .sdio_bus_mhz = 50u, .spi_bus_mhz = 50u};
    TEST_ASSERT_FALSE(pf_boot_config_load(&loaded));
    TEST_ASSERT_EQUAL_UINT16(0u, loaded.system_clock_mhz);
    TEST_ASSERT_EQUAL_UINT8(0u, loaded.sdio_bus_mhz);
    TEST_ASSERT_EQUAL_UINT8(0u, loaded.spi_bus_mhz);
}

static void valid_record(uint8_t record[PF_BOOT_CONFIG_RECORD_SIZE])
{
    pf_boot_config_t settings = {.system_clock_mhz = 200u, .sdio_bus_mhz = 50u, .spi_bus_mhz = 25u};
    TEST_ASSERT_TRUE(pf_boot_config_encode(&settings, record));
}

static void reseal(uint8_t record[PF_BOOT_CONFIG_RECORD_SIZE])
{
    uint32_t crc = pf_boot_config_crc32(record, PF_BOOT_CONFIG_RECORD_SIZE - 4u);
    for (unsigned i = 0u; i < 4u; ++i) {
        record[PF_BOOT_CONFIG_RECORD_SIZE - 4u + i] = (uint8_t)(crc >> (8u * i));
    }
}

static void assert_defaults(const uint8_t *record, size_t size)
{
    pf_boot_config_t settings = {.system_clock_mhz = 250u, .sdio_bus_mhz = 50u, .spi_bus_mhz = 50u};
    TEST_ASSERT_FALSE(pf_boot_config_decode(record, size, &settings));
    TEST_ASSERT_EQUAL_UINT16(0u, settings.system_clock_mhz);
    TEST_ASSERT_EQUAL_UINT8(0u, settings.sdio_bus_mhz);
    TEST_ASSERT_EQUAL_UINT8(0u, settings.spi_bus_mhz);
}

void test_crc32_matches_the_standard_check_value(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xcbf43926u, pf_boot_config_crc32((const uint8_t *)"123456789", 9u));
}

void test_defaults_are_complete_and_valid(void)
{
    pf_boot_config_t settings = {1u, 2u, 3u};
    pf_boot_config_defaults(&settings);
    TEST_ASSERT_EQUAL_UINT16(0u, settings.system_clock_mhz);
    TEST_ASSERT_EQUAL_UINT8(0u, settings.sdio_bus_mhz);
    TEST_ASSERT_EQUAL_UINT8(0u, settings.spi_bus_mhz);
    TEST_ASSERT_TRUE(pf_boot_config_valid(&settings));
}

void test_round_trip_and_fixed_layout(void)
{
    uint8_t record[PF_BOOT_CONFIG_RECORD_SIZE];
    pf_boot_config_t settings;
    valid_record(record);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){'P', 'F', 'C', 'F', 1, 0, 16, 0, 200, 0, 50, 25}), record, 12);
    TEST_ASSERT_TRUE(pf_boot_config_decode(record, sizeof(record), &settings));
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
            uint8_t record[PF_BOOT_CONFIG_RECORD_SIZE];
            pf_boot_config_t in = {clocks[c], buses[b], buses[(b + 1u) % 3u]}, out;
            TEST_ASSERT_TRUE(pf_boot_config_encode(&in, record));
            TEST_ASSERT_TRUE(pf_boot_config_decode(record, sizeof(record), &out));
            TEST_ASSERT_EQUAL_MEMORY(&in, &out, sizeof(in));
        }
    }
    uint8_t record[PF_BOOT_CONFIG_RECORD_SIZE];
    const pf_boot_config_t bad[] = {{100u, 0u, 0u}, {125u, 0u, 0u}, {251u, 0u, 0u}, {0u, 10u, 0u},
                                 {0u, 100u, 0u}, {0u, 0u, 10u}, {0u, 0u, 75u}};
    for (unsigned i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        TEST_ASSERT_FALSE(pf_boot_config_valid(&bad[i]));
        TEST_ASSERT_FALSE(pf_boot_config_encode(&bad[i], record));
    }
}

void test_missing_short_and_erased_data_return_defaults(void)
{
    uint8_t record[PF_BOOT_CONFIG_RECORD_SIZE];
    uint8_t erased[PF_BOOT_CONFIG_RECORD_SIZE];
    pf_boot_config_t settings = {150u, 25u, 25u};
    memset(erased, 0xff, sizeof(erased));
    valid_record(record);
    assert_defaults(NULL, sizeof(record));
    assert_defaults(record, PF_BOOT_CONFIG_RECORD_SIZE - 1u);
    assert_defaults(erased, sizeof(erased));
    TEST_ASSERT_FALSE(pf_boot_config_decode(record, sizeof(record), NULL));
    TEST_ASSERT_TRUE(pf_boot_config_decode(record, sizeof(record), &settings));
}

void test_crc_covers_every_byte_before_it(void)
{
    uint8_t record[PF_BOOT_CONFIG_RECORD_SIZE];
    for (unsigned byte = 0u; byte < PF_BOOT_CONFIG_RECORD_SIZE; ++byte) {
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            valid_record(record);
            record[byte] ^= (uint8_t)(1u << bit);
            assert_defaults(record, sizeof(record));
        }
    }
}

void test_unknown_version_size_magic_are_rejected_even_with_valid_crc(void)
{
    uint8_t record[PF_BOOT_CONFIG_RECORD_SIZE];
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
    uint8_t record[PF_BOOT_CONFIG_RECORD_SIZE];
    for (unsigned i = 0u; i < 4u; ++i) {
        valid_record(record);
        record[cases[i].offset] = cases[i].value;
        reseal(record);
        assert_defaults(record, sizeof(record));
    }
}
