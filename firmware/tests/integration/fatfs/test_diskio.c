#include "unity.h"
#include "ff.h"
#include "diskio.h"
#include "sdio_block.h"

static bool card_ready;
static picoforge_sdio_result_t read_result;
static picoforge_sdio_result_t write_result;
static picoforge_sdio_result_t sync_result;
static uint32_t reported_block_count;
static uint32_t last_first_block;
static uint32_t last_block_count;
static const void *last_buffer;
static uint32_t read_calls;
static uint32_t write_calls;
static uint32_t sync_calls;

bool picoforge_sdio_is_initialized(void)
{
    return card_ready;
}

uint32_t picoforge_sdio_block_count(void)
{
    return reported_block_count;
}

picoforge_sdio_result_t picoforge_sdio_read_blocks(uint32_t first_block,
                                                    void *buffer,
                                                    uint32_t block_count)
{
    last_first_block = first_block;
    last_block_count = block_count;
    last_buffer = buffer;
    read_calls++;
    return read_result;
}

picoforge_sdio_result_t picoforge_sdio_write_blocks(uint32_t first_block,
                                                     const void *buffer,
                                                     uint32_t block_count)
{
    last_first_block = first_block;
    last_block_count = block_count;
    last_buffer = buffer;
    write_calls++;
    return write_result;
}

picoforge_sdio_result_t picoforge_sdio_sync(void)
{
    sync_calls++;
    return sync_result;
}

void setUp(void)
{
    card_ready = true;
    read_result = PICOFORGE_SDIO_OK;
    write_result = PICOFORGE_SDIO_OK;
    sync_result = PICOFORGE_SDIO_OK;
    reported_block_count = 1234u;
    last_first_block = 0u;
    last_block_count = 0u;
    last_buffer = NULL;
    read_calls = 0u;
    write_calls = 0u;
    sync_calls = 0u;
}

void tearDown(void) {}

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

void test_reads_unaligned_buffer_through_aligned_sector_bounce_buffer(void)
{
    uint32_t storage[257];
    BYTE *buffer = (BYTE *)storage + 1u;

    TEST_ASSERT_EQUAL(RES_OK, disk_read(0u, buffer, 4u, 2u));
    TEST_ASSERT_EQUAL_UINT32(2u, read_calls);
    TEST_ASSERT_EQUAL_UINT32(5u, last_first_block);
    TEST_ASSERT_EQUAL_UINT32(1u, last_block_count);
    TEST_ASSERT_NOT_NULL(last_buffer);
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)((uintptr_t)last_buffer & 3u));
}

void test_writes_unaligned_buffer_one_sector_at_a_time(void)
{
    uint32_t storage[129];
    const BYTE *buffer = (const BYTE *)storage + 1u;

    TEST_ASSERT_EQUAL(RES_OK, disk_write(0u, buffer, 9u, 2u));
    TEST_ASSERT_EQUAL_UINT32(2u, write_calls);
    TEST_ASSERT_EQUAL_UINT32(10u, last_first_block);
    TEST_ASSERT_EQUAL_UINT32(1u, last_block_count);
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)((uintptr_t)last_buffer & 3u));
}

void test_translates_unready_and_io_failures(void)
{
    uint32_t aligned_buffer[128];
    BYTE *buffer = (BYTE *)aligned_buffer;

    card_ready = false;
    TEST_ASSERT_EQUAL(RES_NOTRDY, disk_read(0u, buffer, 0u, 1u));
    TEST_ASSERT_EQUAL_UINT32(0u, read_calls);

    card_ready = true;
    read_result = PICOFORGE_SDIO_IO_ERROR;
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
    TEST_ASSERT_EQUAL(PICOFORGE_SDIO_BLOCK_SIZE, sector_size);
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