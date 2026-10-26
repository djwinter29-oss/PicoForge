#include "unity.h"
#define MEDIA_SOURCE "middleware/storage/storage.c"
#include MEDIA_SOURCE
#include "block_device.h"
#include "Mocksd.h"
#include "Mocksd_port.h"
#include "Mocksd_board.h"
#include "Mockfatfs_volume.h"
#include "Mocklogging.h"

uint32_t pf_system_uptime_ms(void) { return 0u; }

static bool mounted, ready;
static unsigned depth, init_calls;
static pf_sd_result_t io_result;
static pf_sd_config_t config;

static void lock(int calls) { (void)calls; ++depth; }
static void unlock(int calls)
{
    (void)calls;
    TEST_ASSERT_GREATER_THAN_UINT(0u, depth);
    --depth;
}
static bool is_mounted(int calls) { (void)calls; return mounted; }
static bool is_ready(int calls) { (void)calls; return ready; }
static pf_sd_result_t initialize(const pf_sd_config_t *value, int calls)
{
    (void)calls;
    TEST_ASSERT_EQUAL_MEMORY(&config, value, sizeof(config));
    TEST_ASSERT_GREATER_THAN_UINT(0u, depth);
    ++init_calls;
    ready = true;
    return PF_SD_OK;
}
static pf_sd_result_t read_blocks(uint32_t first_block, void *buffer, uint32_t block_count, int calls)
{
    (void)calls;
    TEST_ASSERT_GREATER_THAN_UINT(0u, depth);
    TEST_ASSERT_EQUAL_UINT32(1u, first_block);
    TEST_ASSERT_EQUAL_UINT32(2u, block_count);
    TEST_ASSERT_NOT_NULL(buffer);
    return io_result;
}
static pf_sd_result_t write_blocks(uint32_t first_block, const void *buffer, uint32_t block_count, int calls)
{
    return read_blocks(first_block, (void *)buffer, block_count, calls);
}

void setUp(void)
{
    mounted = false;
    ready = true;
    depth = init_calls = 0u;
    io_result = PF_SD_OK;
    config = (pf_sd_config_t){.transport.mode = PF_SD_LINK_NATIVE_SD, .clock_hz = 25000000u};
    configured = true;
    media_config = config;
    pf_sd_lock_Stub(lock);
    pf_sd_unlock_Stub(unlock);
    pf_sd_is_initialized_Stub(is_ready);
    pf_fatfs_volume_is_mounted_Stub(is_mounted);
    pf_sd_init_Stub(initialize);
}

void tearDown(void)
{
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_storage_msc_release());
    reinit_after_usb_owner = false;
    last_failure = PF_STORAGE_OPERATION_NONE;
    last_failure_result = PF_BLOCK_OK;
    TEST_ASSERT_EQUAL_UINT(0u, depth);
}

void test_setup_reports_unavailable_configuration_and_port_failure_then_borrows_board_configuration(void)
{
    configured = false;
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_storage_initialize());
    pf_sd_board_config_ExpectAnyArgsAndReturn(false);
    pf_logging_write_ExpectAnyArgs();
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_storage_setup());
    pf_sd_board_config_ExpectAnyArgsAndReturn(true);
    pf_sd_board_config_ReturnThruPtr_config(&config);
    pf_sd_port_init_ExpectAnyArgsAndReturn(false);
    pf_logging_write_ExpectAnyArgs();
    TEST_ASSERT_EQUAL(PF_BLOCK_IO_ERROR, pf_storage_setup());
    pf_sd_board_config_ExpectAnyArgsAndReturn(true);
    pf_sd_board_config_ReturnThruPtr_config(&config);
    pf_sd_port_init_ExpectAnyArgsAndReturn(true);
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_storage_setup());
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_storage_setup());
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_storage_initialize());
    TEST_ASSERT_EQUAL_UINT(1u, init_calls);
}

void test_mount_and_usb_owner_prevent_reinitialization_and_acquisition_requires_unmounted_media(void)
{
    mounted = true;
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_storage_initialize());
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_storage_msc_acquire());
    mounted = false;
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_storage_msc_acquire());
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_storage_initialize());
    TEST_ASSERT_EQUAL_UINT(0u, init_calls);
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_storage_msc_release_for_recovery());
    TEST_ASSERT_TRUE(usb_owned);
    ready = false;
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_storage_msc_release_for_recovery());
    TEST_ASSERT_TRUE(reinit_after_usb_owner);
    reinit_after_usb_owner = false;
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_storage_msc_release_for_recovery());
}

void test_firmware_descriptor_and_usb_operations_enforce_owner_and_retain_the_last_failure(void)
{
    uint8_t buffer[1024];
    const pf_block_device_t *firmware = pf_storage_firmware_device();
    pf_sd_block_count_IgnoreAndReturn(8u);
    pf_sd_read_blocks_Stub(read_blocks);
    pf_sd_write_blocks_Stub(write_blocks);
    pf_sd_sync_IgnoreAndReturn(PF_SD_OK);
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_storage_msc_read(1u, buffer, 2u));
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_block_read(firmware, 1u, buffer, 2u));
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_storage_msc_acquire());
    pf_block_geometry_t geometry;
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_block_geometry(firmware, &geometry));
    TEST_ASSERT_EQUAL_UINT32(8u, geometry.sector_count);
    TEST_ASSERT_EQUAL_UINT16(512u, geometry.sector_size);
    TEST_ASSERT_EQUAL_UINT32(1u, geometry.erase_sector_count);
    TEST_ASSERT_FALSE(geometry.write_protected);
    ready = false;
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_block_geometry(firmware, &geometry));
    ready = true;
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_block_read(firmware, 1u, buffer, 2u));
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_block_write(firmware, 1u, buffer, 2u));
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_block_sync(firmware));
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_storage_msc_read(1u, buffer, 2u));
    io_result = PF_SD_IO_ERROR;
    TEST_ASSERT_EQUAL(PF_BLOCK_IO_ERROR, pf_storage_msc_write(1u, buffer, 2u));
    TEST_ASSERT_EQUAL(PF_STORAGE_OPERATION_WRITE, last_failure);
    TEST_ASSERT_EQUAL(PF_BLOCK_IO_ERROR, last_failure_result);
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_storage_msc_release());
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_storage_msc_sync());
}

static bool try_lock(int calls) { (void)calls; ++depth; return true; }

static void info_stub(pf_sd_info_t *info, int calls)
{
    (void)calls;
    *info = (pf_sd_info_t){
        .initialized = ready, .presence_known = true, .present = true, .cid_valid = true,
        .bus_clock_hz = 25000000u, .block_count = 64u, .high_speed = true,
    };
}

void test_status_reports_board_fields_without_blocking_and_maps_lifecycle_states(void)
{
    pf_storage_status_t status;
    pf_sd_board_pins_IgnoreAndReturn(PF_SD_LINK_SPI);
    configured = false;
    pf_storage_get_status(&status);
    TEST_ASSERT_EQUAL(PF_STORAGE_STATE_NOT_CONFIGURED, status.state);
    TEST_ASSERT_EQUAL(PF_STORAGE_MEDIUM_SDCARD, status.medium);
    TEST_ASSERT_EQUAL(PF_STORAGE_BUS_SPI, status.bus);
    configured = true;
    pf_sd_try_lock_ExpectAndReturn(false);
    pf_storage_get_status(&status);
    TEST_ASSERT_EQUAL(PF_STORAGE_STATE_BUSY, status.state);

    pf_sd_try_lock_Stub(try_lock);
    pf_sd_get_info_Stub(info_stub);
    pf_storage_get_status(&status);
    TEST_ASSERT_EQUAL(PF_STORAGE_STATE_READY, status.state);
    TEST_ASSERT_TRUE(status.id_valid);
    TEST_ASSERT_TRUE(status.high_speed);
    TEST_ASSERT_EQUAL_UINT32(64u, status.sector_count);
    mounted = true;
    pf_storage_get_status(&status);
    TEST_ASSERT_EQUAL(PF_STORAGE_STATE_MOUNTED, status.state);
    mounted = false;
    usb_owned = true;
    pf_storage_get_status(&status);
    TEST_ASSERT_EQUAL(PF_STORAGE_STATE_USB, status.state);
    usb_owned = false;
    ready = false;
    last_failure = PF_STORAGE_OPERATION_INIT;
    last_failure_result = PF_BLOCK_IO_ERROR;
    pf_storage_get_status(&status);
    TEST_ASSERT_EQUAL(PF_STORAGE_STATE_UNINITIALIZED, status.state);
    TEST_ASSERT_EQUAL(PF_STORAGE_OPERATION_INIT, status.last_failure);
    TEST_ASSERT_EQUAL(PF_BLOCK_IO_ERROR, status.last_failure_result);
    ready = true;
}

static void absent_info(pf_sd_info_t *info, int calls)
{
    (void)calls;
    *info = (pf_sd_info_t){.presence_known = true};
}

void test_removal_unmount_failure_is_reported_and_retried_until_binding_is_released(void)
{
    mounted = true;
    presence_sample_valid = true;
    presence_sample = false;
    presence_sample_count = CARD_PRESENCE_DEBOUNCE_SAMPLES - 1u;
    stable_presence_valid = true;
    stable_card_present = true;
    pf_sd_get_info_Stub(absent_info);
    pf_sd_confirm_removed_Expect();
    pf_fatfs_volume_forget_filesystem_format_Expect();
    pf_fatfs_volume_unmount_ExpectAnyArgsAndReturn(PF_FATFS_VOLUME_FILESYSTEM_ERROR);
    pf_logging_write_ExpectAnyArgs();
    pf_storage_poll();
    TEST_ASSERT_TRUE(mounted);
    TEST_ASSERT_FALSE(pf_storage_msc_acquire() == PF_BLOCK_OK);
    pf_fatfs_volume_unmount_ExpectAnyArgsAndReturn(PF_FATFS_VOLUME_OK);
    pf_storage_poll();
    mounted = false;
    pf_storage_poll();
    TEST_ASSERT_TRUE(stable_presence_valid);
    TEST_ASSERT_FALSE(stable_card_present);
    presence_sample_valid = stable_presence_valid = false;
}
