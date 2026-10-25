#include "unity.h"
#define MEDIA_SOURCE "app/storage/sd_media.c"
#include MEDIA_SOURCE
#include "block_device.h"
#include "sd_block_device.h"
#include "Mocksd.h"
#include "Mocksd_block.h"
#include "Mocksd_port.h"
#include "Mocksd_board.h"
#include "Mockfatfs_volume.h"
#include "Mocklogging.h"

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
    pf_sd_get_init_stage_IgnoreAndReturn(99u);
    pf_sd_get_profile_Ignore();
    pf_fatfs_volume_is_mounted_Stub(is_mounted);
    pf_sd_init_Stub(initialize);
}

void tearDown(void)
{
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_media_release_usb());
    (void)pf_sd_media_take_reinit_request();
    pf_sd_media_clear_diagnostics();
    TEST_ASSERT_EQUAL_UINT(0u, depth);
}

void test_setup_reports_unavailable_configuration_and_port_failure_then_borrows_board_configuration(void)
{
    configured = false;
    TEST_ASSERT_EQUAL(PF_SD_NOT_CONFIGURED, pf_sd_media_initialize());
    pf_sd_board_config_ExpectAnyArgsAndReturn(false);
    pf_log_write_ExpectAnyArgs();
    TEST_ASSERT_EQUAL(PF_SD_NOT_CONFIGURED, pf_sd_media_setup());
    pf_sd_board_config_ExpectAnyArgsAndReturn(true);
    pf_sd_board_config_ReturnThruPtr_config(&config);
    pf_sd_port_init_ExpectAnyArgsAndReturn(false);
    pf_log_write_ExpectAnyArgs();
    TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, pf_sd_media_setup());
    pf_sd_board_config_ExpectAnyArgsAndReturn(true);
    pf_sd_board_config_ReturnThruPtr_config(&config);
    pf_sd_port_init_ExpectAnyArgsAndReturn(true);
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_media_setup());
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_media_setup());
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_media_initialize());
    TEST_ASSERT_EQUAL_UINT(1u, init_calls);
}

void test_mount_and_usb_owner_prevent_reinitialization_and_acquisition_requires_unmounted_media(void)
{
    mounted = true;
    TEST_ASSERT_EQUAL(PF_SD_NOT_READY, pf_sd_media_initialize());
    TEST_ASSERT_EQUAL(PF_SD_NOT_READY, pf_sd_media_acquire_usb());
    mounted = false;
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_media_acquire_usb());
    TEST_ASSERT_EQUAL(PF_SD_NOT_READY, pf_sd_media_initialize());
    TEST_ASSERT_EQUAL_UINT(0u, init_calls);
    TEST_ASSERT_EQUAL(PF_SD_NOT_READY, pf_sd_media_release_usb_for_recovery());
    TEST_ASSERT_EQUAL(PF_SD_MEDIA_OWNER_USB_MSC, pf_sd_media_get_owner());
    ready = false;
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_media_release_usb_for_recovery());
    TEST_ASSERT_TRUE(pf_sd_media_take_reinit_request());
    TEST_ASSERT_FALSE(pf_sd_media_take_reinit_request());
    TEST_ASSERT_EQUAL(PF_SD_NOT_READY, pf_sd_media_release_usb_for_recovery());
}

void test_firmware_descriptor_and_usb_operations_enforce_owner_and_preserve_failed_write_diagnostics(void)
{
    uint8_t buffer[1024];
    const pf_block_device_t *firmware = pf_sd_media_firmware_device();
    pf_sd_block_count_IgnoreAndReturn(8u);
    pf_sd_read_blocks_Stub(read_blocks);
    pf_sd_write_blocks_Stub(write_blocks);
    pf_sd_sync_IgnoreAndReturn(PF_SD_OK);
    TEST_ASSERT_EQUAL(PF_SD_NOT_READY, pf_sd_media_usb_read_blocks(1u, buffer, 2u));
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_block_read(firmware, 1u, buffer, 2u));
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_media_acquire_usb());
    pf_block_geometry_t geometry;
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_block_geometry(firmware, &geometry));
    TEST_ASSERT_EQUAL_UINT32(8u, geometry.sector_count);
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_block_read(firmware, 1u, buffer, 2u));
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_block_write(firmware, 1u, buffer, 2u));
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_block_sync(firmware));
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_media_usb_read_blocks(1u, buffer, 2u));
    io_result = PF_SD_IO_ERROR;
    TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, pf_sd_media_usb_write_blocks(1u, buffer, 2u));
    pf_sd_media_diagnostics_t diagnostic;
    pf_sd_media_get_diagnostics(&diagnostic);
    TEST_ASSERT_EQUAL(PF_SD_MEDIA_OPERATION_WRITE, diagnostic.operation);
    TEST_ASSERT_EQUAL(PF_SD_MEDIA_OWNER_USB_MSC, diagnostic.owner);
    TEST_ASSERT_EQUAL(PF_SD_IO_ERROR, diagnostic.result);
    TEST_ASSERT_EQUAL_UINT32(99u, diagnostic.transport_init_stage);
    uint32_t sequence = diagnostic.sequence;
    pf_sd_media_clear_diagnostics();
    pf_sd_media_get_diagnostics(&diagnostic);
    TEST_ASSERT_EQUAL_UINT32(sequence, diagnostic.sequence);
    TEST_ASSERT_EQUAL(PF_SD_MEDIA_OPERATION_NONE, diagnostic.operation);
    TEST_ASSERT_EQUAL(PF_SD_OK, pf_sd_media_release_usb());
    TEST_ASSERT_EQUAL(PF_SD_NOT_READY, pf_sd_media_usb_sync());
}
