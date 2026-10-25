#include "unity.h"

#include <string.h>

TEST_INCLUDE_PATH("firmware/tests/support/config")

#define BOOT_CONFIG_SOURCE "drivers/boot_config.c"
#include BOOT_CONFIG_SOURCE

static record_t stored;
static uint8_t *const stored_bytes = (uint8_t *)&stored;
static bool before_scheduler;
static bool read_ok;
static bool write_ok;
static unsigned writes;

bool pf_platform_boot_config_read_early(uint8_t *data, size_t size)
{
    if (!before_scheduler) {
        return false;
    }
    memcpy(data, &stored, size);
    return true;
}

bool pf_platform_boot_config_read(uint8_t *data, size_t size)
{
    if (before_scheduler || !read_ok) {
        return false;
    }
    memcpy(data, &stored, size);
    return true;
}

bool pf_platform_boot_config_write(const uint8_t *data, size_t size)
{
    ++writes;
    if (write_ok) {
        memcpy(&stored, data, size);
    }
    return write_ok;
}

static void reseal(void)
{
    stored.crc = crc32(&stored, offsetof(record_t, crc));
}

static void store(uint16_t sysclk_mhz, uint8_t sdio_mhz, uint8_t spi_mhz)
{
    const uint8_t header[] = {'P', 'F', 'C', 'F', 1, 0, 16, 0};
    memcpy(stored_bytes, header, sizeof(header));
    stored.sysclk_mhz = sysclk_mhz;
    stored.sdio_bus_mhz = sdio_mhz;
    stored.spi_bus_mhz = spi_mhz;
    reseal();
}

static void reboot(void)
{
    loaded = false;
    config = (boot_config_t){0};
    before_scheduler = true;
}

void setUp(void)
{
    memset(&stored, 0xff, sizeof(stored));
    read_ok = write_ok = true;
    writes = 0u;
    reboot();
}

void tearDown(void) {}

void test_profile_conversions_report_mhz_and_fall_back_for_unknown_values(void)
{
    TEST_ASSERT_EQUAL_UINT16(200u, pf_sysclk_profile_mhz(PF_SYSCLK_200MHZ));
    TEST_ASSERT_EQUAL_UINT16(0u, pf_sysclk_profile_mhz((pf_sysclk_profile_t)4));
    TEST_ASSERT_EQUAL_UINT8(25u, pf_storage_speed_mhz(PF_STORAGE_SPEED_DEFAULT));
    TEST_ASSERT_EQUAL_UINT8(50u, pf_storage_speed_mhz(PF_STORAGE_SPEED_HIGH));
    TEST_ASSERT_EQUAL_UINT8(25u, pf_storage_speed_mhz((pf_storage_speed_t)2));
}

void test_crc32_matches_the_standard_check_value(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xcbf43926u, crc32((const uint8_t *)"123456789", 9u));
}

void test_configured_mhz_getters_load_defaults_saved_profiles_and_ram_updates(void)
{
    TEST_ASSERT_EQUAL_UINT16(0u, pf_get_sysclk_mhz());
    TEST_ASSERT_EQUAL_UINT8(25u, pf_get_storage_speed_mhz());
    store(200u, 50u, 50u);
    reboot();
    TEST_ASSERT_EQUAL_UINT8(50u, pf_get_storage_speed_mhz());
    TEST_ASSERT_EQUAL_UINT16(200u, pf_get_sysclk_mhz());
    TEST_ASSERT_TRUE(pf_boot_config_set_sysclk(PF_SYSCLK_150MHZ));
    TEST_ASSERT_TRUE(pf_boot_config_set_storage_speed(PF_STORAGE_SPEED_DEFAULT));
    TEST_ASSERT_EQUAL_UINT16(150u, pf_get_sysclk_mhz());
    TEST_ASSERT_EQUAL_UINT8(25u, pf_get_storage_speed_mhz());
    TEST_ASSERT_EQUAL_UINT(0u, writes);
}

void test_erased_storage_gives_sdk_clock_and_default_sd_speed(void)
{
    TEST_ASSERT_EQUAL(PF_SYSCLK_DEFAULT, pf_boot_config_sysclk());
    TEST_ASSERT_EQUAL(PF_STORAGE_SPEED_DEFAULT, pf_boot_config_storage_speed());
}

void test_early_load_reads_the_saved_record(void)
{
    store(200u, 50u, 50u);
    TEST_ASSERT_EQUAL(PF_SYSCLK_200MHZ, pf_boot_config_sysclk());
    TEST_ASSERT_EQUAL(PF_STORAGE_SPEED_HIGH, pf_boot_config_storage_speed());
}

void test_legacy_single_transport_record_reads_high_from_either_field(void)
{
    store(0u, 50u, 0u);
    TEST_ASSERT_EQUAL(PF_STORAGE_SPEED_HIGH, pf_boot_config_storage_speed());
    reboot();
    store(0u, 0u, 50u);
    TEST_ASSERT_EQUAL(PF_STORAGE_SPEED_HIGH, pf_boot_config_storage_speed());
}

void test_legacy_zero_sd_field_reads_as_default(void)
{
    store(150u, 0u, 0u);
    TEST_ASSERT_EQUAL(PF_SYSCLK_150MHZ, pf_boot_config_sysclk());
    TEST_ASSERT_EQUAL(PF_STORAGE_SPEED_DEFAULT, pf_boot_config_storage_speed());
}

void test_runtime_first_access_falls_back_to_the_task_read(void)
{
    store(250u, 50u, 50u);
    before_scheduler = false;
    TEST_ASSERT_EQUAL(PF_SYSCLK_250MHZ, pf_boot_config_sysclk());
}

void test_setters_reject_unsupported_values_without_changing_the_configuration(void)
{
    TEST_ASSERT_FALSE(pf_boot_config_set_sysclk((pf_sysclk_profile_t)4));
    TEST_ASSERT_FALSE(pf_boot_config_set_sysclk((pf_sysclk_profile_t)-1));
    TEST_ASSERT_FALSE(pf_boot_config_set_storage_speed((pf_storage_speed_t)2));
    TEST_ASSERT_EQUAL(PF_SYSCLK_DEFAULT, pf_boot_config_sysclk());
    TEST_ASSERT_EQUAL(PF_STORAGE_SPEED_DEFAULT, pf_boot_config_storage_speed());
}

void test_save_writes_the_v1_layout_and_survives_reboot(void)
{
    const uint16_t clocks[] = {0u, 150u, 200u, 250u};
    for (unsigned i = 0u; i < 4u; ++i) {
        for (unsigned speed = 0u; speed < 2u; ++speed) {
            reboot();
            before_scheduler = false;
            TEST_ASSERT_TRUE(pf_boot_config_set_sysclk((pf_sysclk_profile_t)i));
            TEST_ASSERT_TRUE(pf_boot_config_set_storage_speed((pf_storage_speed_t)speed));
            TEST_ASSERT_TRUE(pf_boot_config_save());
            const uint8_t sd_mhz = speed != 0u ? 50u : 25u;
            TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){'P', 'F', 'C', 'F', 1, 0, 16, 0,
                                                      (uint8_t)clocks[i], (uint8_t)(clocks[i] >> 8), sd_mhz, sd_mhz}),
                                         stored_bytes, 12);
            reboot();
            TEST_ASSERT_EQUAL(i, pf_boot_config_sysclk());
            TEST_ASSERT_EQUAL(speed, pf_boot_config_storage_speed());
        }
    }
}

void test_failed_write_is_reported(void)
{
    write_ok = false;
    TEST_ASSERT_TRUE(pf_boot_config_set_sysclk(PF_SYSCLK_150MHZ));
    TEST_ASSERT_FALSE(pf_boot_config_save());
    TEST_ASSERT_EQUAL_UINT(1u, writes);
}

void test_corrupt_or_unknown_records_give_defaults(void)
{
    const struct { unsigned offset; uint8_t value; bool reseal; } cases[] = {
        {0u, 'X', true}, {4u, 2u, true}, {6u, 15u, true}, {8u, 100u, true}, {9u, 1u, true},
        {10u, 40u, true}, {11u, 75u, true}, {8u, 150u, false}, {15u, 0u, false},
    };
    for (unsigned i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        store(200u, 50u, 50u);
        stored_bytes[cases[i].offset] = cases[i].value;
        if (cases[i].reseal) {
            reseal();
        }
        reboot();
        TEST_ASSERT_EQUAL(PF_SYSCLK_DEFAULT, pf_boot_config_sysclk());
        TEST_ASSERT_EQUAL(PF_STORAGE_SPEED_DEFAULT, pf_boot_config_storage_speed());
    }
}
