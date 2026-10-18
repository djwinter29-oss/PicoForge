#include "unity.h"
#include "ff.h"
#include "diskio.h"
#include "fatfs_disk.h"
#include "block_device.h"

static bool card_ready;
static pf_block_result_t read_result;
static pf_block_result_t write_result;
static pf_block_result_t sync_result;
static bool write_protected;
static uint16_t sector_size;
static uint32_t reported_block_count;
static uint32_t last_first_block;
static uint32_t last_block_count;
static const void *last_buffer;
static uint32_t read_calls;
static uint32_t write_calls;
static uint32_t sync_calls;

static pf_block_result_t device_geometry(void *context, pf_block_geometry_t *geometry)
{
    (void)context;
    if (!card_ready) return PF_BLOCK_NOT_READY;
    *geometry = (pf_block_geometry_t){
        .sector_count = reported_block_count, .sector_size = sector_size,
        .erase_sector_count = 1u, .write_protected = write_protected,
    };
    return PF_BLOCK_OK;
}

static pf_block_result_t device_read(void *context, uint32_t first_block,
                                           void *buffer,
                                           uint32_t block_count)
{
    (void)context;
    last_first_block = first_block;
    last_block_count = block_count;
    last_buffer = buffer;
    read_calls++;
    return read_result;
}

static pf_block_result_t device_write(void *context, uint32_t first_block,
                                            const void *buffer,
                                            uint32_t block_count)
{
    (void)context;
    last_first_block = first_block;
    last_block_count = block_count;
    last_buffer = buffer;
    write_calls++;
    return write_result;
}

static pf_block_result_t device_sync(void *context)
{
    (void)context;
    sync_calls++;
    return sync_result;
}

void setUp(void)
{
    card_ready = true;
    read_result = PF_BLOCK_OK;
    write_result = PF_BLOCK_OK;
    sync_result = PF_BLOCK_OK;
    write_protected = false;
    sector_size = 512u;
    static const pf_block_device_ops_t ops = {
        .geometry = device_geometry, .read = device_read, .write = device_write, .sync = device_sync,
    };
    static const pf_block_device_t device = {.ops = &ops};
    pf_fatfs_bind_device(&device);
    reported_block_count = 1234u;
    last_first_block = 0u;
    last_block_count = 0u;
    last_buffer = NULL;
    read_calls = 0u;
    write_calls = 0u;
    sync_calls = 0u;
}

void tearDown(void) { pf_fatfs_bind_device(NULL); }

void test_status_reports_drive_zero_readiness(void)
{
    TEST_ASSERT_EQUAL_UINT8(0u, disk_initialize(0u));
    card_ready = false;
    TEST_ASSERT_EQUAL_UINT8(STA_NOINIT, disk_status(0u));
    TEST_ASSERT_EQUAL_UINT8(STA_NOINIT, disk_initialize(1u));
}

void test_reads_aligned_buffer_in_one_block_request(void)
{
    uint32_t aligned_buffer[128];
    BYTE *buffer = (BYTE *)aligned_buffer;

    TEST_ASSERT_EQUAL(RES_OK, disk_read(0u, buffer, 7u, 2u));
    TEST_ASSERT_EQUAL_UINT32(1u, read_calls);
    TEST_ASSERT_EQUAL_UINT32(7u, last_first_block);
    TEST_ASSERT_EQUAL_UINT32(2u, last_block_count);
    TEST_ASSERT_EQUAL_PTR(buffer, last_buffer);
}

void test_passes_unaligned_buffers_straight_through(void)
{
    uint32_t storage[257];
    BYTE *buffer = (BYTE *)storage + 1u;

    TEST_ASSERT_EQUAL(RES_OK, disk_read(0u, buffer, 4u, 2u));
    TEST_ASSERT_EQUAL_UINT32(1u, read_calls);
    TEST_ASSERT_EQUAL_UINT32(4u, last_first_block);
    TEST_ASSERT_EQUAL_UINT32(2u, last_block_count);
    TEST_ASSERT_EQUAL_PTR(buffer, last_buffer);

    TEST_ASSERT_EQUAL(RES_OK, disk_write(0u, buffer, 9u, 3u));
    TEST_ASSERT_EQUAL_UINT32(1u, write_calls);
    TEST_ASSERT_EQUAL_UINT32(9u, last_first_block);
    TEST_ASSERT_EQUAL_UINT32(3u, last_block_count);
    TEST_ASSERT_EQUAL_PTR(buffer, last_buffer);
}

void test_translates_unready_and_io_failures(void)
{
    uint32_t aligned_buffer[128];
    BYTE *buffer = (BYTE *)aligned_buffer;

    card_ready = false;
    TEST_ASSERT_EQUAL(RES_NOTRDY, disk_read(0u, buffer, 0u, 1u));
    TEST_ASSERT_EQUAL_UINT32(0u, read_calls);

    card_ready = true;
    read_result = PF_BLOCK_IO_ERROR;
    TEST_ASSERT_EQUAL(RES_ERROR, disk_read(0u, buffer, 0u, 1u));
}

void test_rejects_invalid_read_and_write_requests(void)
{
    uint32_t aligned_buffer[128];
    BYTE *buffer = (BYTE *)aligned_buffer;

    TEST_ASSERT_EQUAL(RES_PARERR, disk_read(1u, buffer, 0u, 1u));
    TEST_ASSERT_EQUAL(RES_PARERR, disk_read(0u, NULL, 0u, 1u));
    TEST_ASSERT_EQUAL(RES_PARERR, disk_read(0u, buffer, 0u, 0u));
    TEST_ASSERT_EQUAL(RES_PARERR, disk_read(0u, buffer, (LBA_t)UINT32_MAX, 2u));
    TEST_ASSERT_EQUAL(RES_PARERR, disk_write(1u, buffer, 0u, 1u));
    TEST_ASSERT_EQUAL(RES_PARERR, disk_write(0u, NULL, 0u, 1u));
    TEST_ASSERT_EQUAL_UINT32(0u, write_calls);
}

void test_reports_sector_geometry_and_sync(void)
{
    LBA_t sectors = 0u;
    WORD sector_size = 0u;
    DWORD block_size = 0u;

    TEST_ASSERT_EQUAL(RES_OK, disk_ioctl(0u, GET_SECTOR_COUNT, &sectors));
    TEST_ASSERT_EQUAL_UINT32(1234u, (uint32_t)sectors);
    TEST_ASSERT_EQUAL(RES_OK, disk_ioctl(0u, GET_SECTOR_SIZE, &sector_size));
    TEST_ASSERT_EQUAL(512u, sector_size);
    TEST_ASSERT_EQUAL(RES_OK, disk_ioctl(0u, GET_BLOCK_SIZE, &block_size));
    TEST_ASSERT_EQUAL_UINT32(1u, block_size);
    TEST_ASSERT_EQUAL(RES_OK, disk_ioctl(0u, CTRL_SYNC, NULL));
    TEST_ASSERT_EQUAL_UINT32(1u, sync_calls);
}

void test_rejects_unsupported_or_malformed_ioctl_requests(void)
{
    TEST_ASSERT_EQUAL(RES_PARERR, disk_ioctl(1u, CTRL_SYNC, NULL));
    TEST_ASSERT_EQUAL(RES_PARERR, disk_ioctl(0u, GET_SECTOR_COUNT, NULL));
    card_ready = false;
    TEST_ASSERT_EQUAL(RES_NOTRDY, disk_ioctl(0u, CTRL_SYNC, NULL));
    card_ready = true;
    TEST_ASSERT_EQUAL(RES_PARERR, disk_ioctl(0u, 0xffu, NULL));
}

void test_unbound_incompatible_and_empty_devices_are_not_ready(void)
{
    BYTE buffer[512];
    sector_size = 4096u;
    TEST_ASSERT_EQUAL(STA_NOINIT, disk_status(0u));
    TEST_ASSERT_EQUAL(RES_NOTRDY, disk_read(0u, buffer, 0u, 1u));
    sector_size = 512u;
    reported_block_count = 0u;
    TEST_ASSERT_EQUAL(STA_NOINIT, disk_status(0u));
    pf_fatfs_bind_device(NULL);
    TEST_ASSERT_EQUAL(STA_NOINIT, disk_status(0u));
    TEST_ASSERT_EQUAL(RES_NOTRDY, disk_ioctl(0u, CTRL_SYNC, NULL));
}

void test_read_only_device_allows_reads_but_rejects_writes_without_dispatch(void)
{
    BYTE buffer[512];
    write_protected = true;
    TEST_ASSERT_EQUAL(STA_PROTECT, disk_status(0u));
    TEST_ASSERT_EQUAL(RES_OK, disk_read(0u, buffer, 0u, 1u));
    TEST_ASSERT_EQUAL(RES_WRPRT, disk_write(0u, buffer, 0u, 1u));
    TEST_ASSERT_EQUAL_UINT(0u, write_calls);
}

void test_geometry_bounds_reject_out_of_range_batches_without_dispatch(void)
{
    BYTE buffer[512];
    TEST_ASSERT_EQUAL(RES_PARERR, disk_read(0u, buffer, reported_block_count, 1u));
    TEST_ASSERT_EQUAL(RES_PARERR, disk_write(0u, buffer, reported_block_count - 1u, 2u));
    TEST_ASSERT_EQUAL_UINT(0u, read_calls + write_calls);
}

void test_operation_errors_propagate_without_retry_or_sync_after_failed_write(void)
{
    BYTE buffer[512];
    const pf_block_result_t errors[] = {
        PF_BLOCK_NOT_READY, PF_BLOCK_INVALID_ARGUMENT, PF_BLOCK_WRITE_PROTECTED, PF_BLOCK_IO_ERROR,
    };
    const DRESULT expected[] = {RES_NOTRDY, RES_PARERR, RES_WRPRT, RES_ERROR};
    for (unsigned i = 0u; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        read_result = write_result = sync_result = errors[i];
        TEST_ASSERT_EQUAL(expected[i], disk_read(0u, buffer, 0u, 1u));
        TEST_ASSERT_EQUAL(expected[i], disk_write(0u, buffer, 0u, 1u));
        TEST_ASSERT_EQUAL_UINT(i, sync_calls);
        TEST_ASSERT_EQUAL(expected[i], disk_ioctl(0u, CTRL_SYNC, NULL));
        TEST_ASSERT_EQUAL_UINT(i + 1u, read_calls);
        TEST_ASSERT_EQUAL_UINT(i + 1u, write_calls);
        TEST_ASSERT_EQUAL_UINT(i + 1u, sync_calls);
    }
}