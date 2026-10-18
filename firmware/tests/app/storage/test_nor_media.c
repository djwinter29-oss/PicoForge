#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/msc")
TEST_INCLUDE_PATH("firmware/src/integration/levelx")
TEST_INCLUDE_PATH("firmware/third_party/levelx/common/inc")
#include "app/storage/nor_media.h"
#include "levelx_block_device.h"
#include "block_device.h"
#include "Mockboard_storage.h"
#include "Mockbus.h"
#include "Mocknor_transport.h"
#include "Mocknor.h"
#include "Mocklevelx_nor.h"
#include "Mockfatfs_volume.h"
#include "Mocklogging.h"

static pf_levelx_nor_t *opened;
static pf_nor_result_t init_result;
static UINT open_result;
static bool mounted;
static unsigned init_calls, open_calls;

static bool is_mounted(int count)
{
    (void)count;
    return mounted;
}

static pf_nor_result_t initialize(pf_nor_t *flash, const pf_nor_transport_t *transport, int count)
{
    (void)transport;
    (void)count;
    ++init_calls;
    flash->initialized = init_result == PF_NOR_OK;
    return init_result;
}

static UINT open_volume(pf_levelx_nor_t *volume, pf_nor_t *flash,
                         uint32_t offset, uint32_t size, int count)
{
    (void)count;
    ++open_calls;
    TEST_ASSERT_TRUE(flash->initialized);
    TEST_ASSERT_EQUAL_UINT(4096u, offset);
    TEST_ASSERT_EQUAL_UINT(32768u, size);
    opened = volume;
    *volume = (pf_levelx_nor_t){
        .opened = open_result == LX_SUCCESS, .sector_count = 42u, .flash = flash,
    };
    return open_result;
}

static UINT close_volume(pf_levelx_nor_t *volume, int count)
{
    (void)count;
    volume->opened = false;
    return LX_SUCCESS;
}

void setUp(void)
{
    mounted = false;
    init_result = PF_NOR_OK;
    open_result = LX_SUCCESS;
    init_calls = open_calls = 0u;
    pf_storage_bus_deinit_Ignore();
    pf_board_storage_prepare_IgnoreAndReturn(PF_STORAGE_BUS_OK);
    pf_nor_transport_init_IgnoreAndReturn(PF_NOR_OK);
    pf_nor_init_Stub(initialize);
    pf_nor_enable_quad_IgnoreAndReturn(PF_NOR_OK);
    pf_levelx_nor_open_Stub(open_volume);
    pf_levelx_nor_close_Stub(close_volume);
    pf_fatfs_volume_is_mounted_Stub(is_mounted);
    TEST_ASSERT_TRUE(pf_nor_media_setup());
}

void tearDown(void)
{
    mounted = false;
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_nor_media_release_usb());
    (void)pf_nor_media_take_remount_request();
    if (opened != NULL) {
        opened->opened = false;
    }
    TEST_ASSERT_TRUE(pf_nor_media_try_lock());
    pf_nor_media_unlock();
}

void test_only_the_configured_region_is_opened_and_live_media_is_not_reinitialized(void)
{
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_nor_media_initialize());
    TEST_ASSERT_EQUAL_UINT(1u, init_calls);
    TEST_ASSERT_EQUAL_UINT(1u, open_calls);
    TEST_ASSERT_TRUE(pf_nor_media_ready());
    TEST_ASSERT_EQUAL_UINT(42u, pf_nor_media_sector_count());
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_nor_media_initialize());
    TEST_ASSERT_EQUAL_UINT(1u, init_calls);
    mounted = true;
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_nor_media_initialize());
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_nor_media_acquire_usb());
}

void test_owner_gates_firmware_and_usb_io_and_release_publishes_one_remount(void)
{
    uint8_t sector[512];
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_nor_media_initialize());
    const pf_block_device_t *firmware = pf_nor_media_firmware_device();
    const pf_block_device_t *host = pf_nor_media_usb_device();
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_block_write(host, 0u, sector, 1u));
    pf_levelx_nor_write_ExpectAndReturn(opened, 0u, sector, LX_SUCCESS);
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_block_write(firmware, 0u, sector, 1u));
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_nor_media_acquire_usb());
    TEST_ASSERT_TRUE(pf_nor_media_usb_owned());
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_nor_media_initialize());
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_block_read(firmware, 0u, sector, 1u));
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_block_write(firmware, 0u, sector, 1u));
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_block_sync(firmware));
    pf_levelx_nor_read_ExpectAndReturn(opened, 0u, sector, LX_SUCCESS);
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_block_read(host, 0u, sector, 1u));
    pf_levelx_nor_sync_ExpectAndReturn(opened, LX_ERROR);
    TEST_ASSERT_EQUAL(PF_BLOCK_IO_ERROR, pf_block_sync(host));
    TEST_ASSERT_TRUE(pf_nor_media_usb_owned());
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_nor_media_release_usb());
    TEST_ASSERT_TRUE(pf_nor_media_take_remount_request());
    TEST_ASSERT_FALSE(pf_nor_media_take_remount_request());
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_block_read(host, 0u, sector, 1u));
}

void test_initialization_and_open_errors_are_explicit_and_fault_recovery_reopens(void)
{
    init_result = PF_NOR_IO_ERROR;
    pf_log_write_ExpectAnyArgs();
    TEST_ASSERT_EQUAL(PF_BLOCK_IO_ERROR, pf_nor_media_initialize());
    TEST_ASSERT_EQUAL_UINT(0u, open_calls);
    init_result = PF_NOR_OK;
    open_result = LX_ERROR;
    pf_log_write_ExpectAnyArgs();
    TEST_ASSERT_EQUAL(PF_BLOCK_IO_ERROR, pf_nor_media_initialize());
    TEST_ASSERT_FALSE(pf_nor_media_ready());
    open_result = LX_SUCCESS;
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_nor_media_initialize());
    opened->faulted = true;
    TEST_ASSERT_FALSE(pf_nor_media_ready());
    TEST_ASSERT_EQUAL_UINT(0u, pf_nor_media_sector_count());
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_nor_media_initialize());
    TEST_ASSERT_TRUE(pf_nor_media_ready());
    TEST_ASSERT_EQUAL_UINT(3u, open_calls);
}

void test_recovery_release_requires_faulted_usb_media_and_never_syncs(void)
{
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_nor_media_initialize());
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_nor_media_release_usb_for_recovery());
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_nor_media_acquire_usb());
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_nor_media_release_usb_for_recovery());
    TEST_ASSERT_TRUE(pf_nor_media_usb_owned());
    opened->faulted = true;
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_nor_media_release_usb_for_recovery());
    TEST_ASSERT_FALSE(pf_nor_media_usb_owned());
    TEST_ASSERT_TRUE(pf_nor_media_take_remount_request());
}
