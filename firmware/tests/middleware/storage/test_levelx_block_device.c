#include "unity.h"
TEST_INCLUDE_PATH("firmware/src/integration/levelx")
TEST_INCLUDE_PATH("firmware/third_party/levelx/common/inc")
#include "levelx_block_device.h"
#include "block_device.h"
#include "Mocklevelx_nor.h"

#include <string.h>

static pf_levelx_nor_t volume;
static pf_block_device_t device;
static unsigned calls;
static unsigned fail_at;

static UINT read_sector(pf_levelx_nor_t *value, uint32_t sector, void *buffer, int count)
{
    (void)count;
    TEST_ASSERT_EQUAL_PTR(&volume, value);
    ++calls;
    if (calls == fail_at) {
        return LX_ERROR;
    }
    memset(buffer, (int)sector, 512u);
    return LX_SUCCESS;
}

static UINT write_sector(pf_levelx_nor_t *value, uint32_t sector, const void *buffer, int count)
{
    (void)count;
    TEST_ASSERT_EQUAL_PTR(&volume, value);
    TEST_ASSERT_EQUAL_UINT8(sector, *(const uint8_t *)buffer);
    ++calls;
    return calls == fail_at ? LX_ERROR : LX_SUCCESS;
}

void setUp(void)
{
    volume = (pf_levelx_nor_t){.opened = true, .sector_count = 16u};
    device = pf_levelx_block_device(&volume);
    calls = fail_at = 0u;
}
void tearDown(void) {}

void test_geometry_reports_logical_not_physical_erase_units_and_readiness(void)
{
    pf_block_geometry_t geometry;
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_block_geometry(&device, &geometry));
    TEST_ASSERT_EQUAL_UINT(16u, geometry.sector_count);
    TEST_ASSERT_EQUAL_UINT(512u, geometry.sector_size);
    TEST_ASSERT_EQUAL_UINT(1u, geometry.erase_sector_count);
    volume.faulted = true;
    TEST_ASSERT_EQUAL(PF_BLOCK_IO_ERROR, pf_block_geometry(&device, &geometry));
    volume.opened = false;
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_block_geometry(&device, &geometry));
    device = pf_levelx_block_device(NULL);
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_block_geometry(&device, &geometry));
}

void test_batches_preserve_unaligned_bytes_and_sync_once(void)
{
    uint8_t bytes[1537];
    pf_levelx_nor_read_Stub(read_sector);
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_block_read(&device, 2u, bytes + 1u, 3u));
    TEST_ASSERT_EQUAL_UINT(3u, calls);
    TEST_ASSERT_EQUAL_UINT8(2u, bytes[1u]);
    TEST_ASSERT_EQUAL_UINT8(3u, bytes[513u]);
    TEST_ASSERT_EQUAL_UINT8(4u, bytes[1025u]);
    calls = 0u;
    pf_levelx_nor_write_Stub(write_sector);
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_block_write(&device, 2u, bytes + 1u, 3u));
    TEST_ASSERT_EQUAL_UINT(3u, calls);
    pf_levelx_nor_sync_ExpectAndReturn(&volume, LX_SUCCESS);
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_block_sync(&device));
}

void test_unmapped_sectors_read_as_erased_but_actual_errors_are_not_hidden(void)
{
    uint8_t bytes[512];
    uint8_t erased[512];
    memset(erased, 0xff, sizeof(erased));
    pf_levelx_nor_read_ExpectAndReturn(&volume, 0u, bytes, LX_SECTOR_NOT_FOUND);
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_block_read(&device, 0u, bytes, 1u));
    TEST_ASSERT_EQUAL_MEMORY(erased, bytes, sizeof(bytes));
    pf_levelx_nor_read_ExpectAndReturn(&volume, 0u, bytes, LX_ERROR);
    TEST_ASSERT_EQUAL(PF_BLOCK_IO_ERROR, pf_block_read(&device, 0u, bytes, 1u));
}

void test_failed_batch_stops_without_retry_or_sync_and_ranges_fail_before_io(void)
{
    uint8_t bytes[1536];
    memset(bytes, 2, 512u);
    memset(bytes + 512u, 3, 512u);
    memset(bytes + 1024u, 4, 512u);
    fail_at = 2u;
    pf_levelx_nor_write_Stub(write_sector);
    TEST_ASSERT_EQUAL(PF_BLOCK_IO_ERROR, pf_block_write(&device, 2u, bytes, 3u));
    TEST_ASSERT_EQUAL_UINT(2u, calls);
    calls = 0u;
    pf_levelx_nor_read_Stub(read_sector);
    TEST_ASSERT_EQUAL(PF_BLOCK_IO_ERROR, pf_block_read(&device, 2u, bytes, 3u));
    TEST_ASSERT_EQUAL_UINT(2u, calls);
    TEST_ASSERT_EQUAL(PF_BLOCK_INVALID_ARGUMENT, pf_block_write(&device, 15u, bytes, 2u));
    TEST_ASSERT_EQUAL(PF_BLOCK_INVALID_ARGUMENT, device.ops->read(device.context, 0u, NULL, 1u));
    TEST_ASSERT_EQUAL(PF_BLOCK_INVALID_ARGUMENT, device.ops->write(device.context, 0u, bytes, 0u));
    TEST_ASSERT_EQUAL_UINT(2u, calls);
    pf_levelx_nor_sync_ExpectAndReturn(&volume, LX_ERROR);
    TEST_ASSERT_EQUAL(PF_BLOCK_IO_ERROR, pf_block_sync(&device));
}
