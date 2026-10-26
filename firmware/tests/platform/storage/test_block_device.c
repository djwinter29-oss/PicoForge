#include "unity.h"
#include "block_device.h"

static pf_block_geometry_t media;
static pf_block_result_t outcome;
static unsigned reads, writes, syncs;
static uint32_t last_first, last_count;
static const void *last_buffer;

static pf_block_result_t geometry(void *context, pf_block_geometry_t *value)
{
    TEST_ASSERT_EQUAL_PTR(&media, context);
    *value = media;
    return outcome;
}

static pf_block_result_t read_blocks(void *context, uint32_t first, void *buffer, uint32_t count)
{
    TEST_ASSERT_EQUAL_PTR(&media, context);
    ++reads;
    last_first = first;
    last_count = count;
    last_buffer = buffer;
    return outcome;
}

static pf_block_result_t write_blocks(void *context, uint32_t first, const void *buffer, uint32_t count)
{
    TEST_ASSERT_EQUAL_PTR(&media, context);
    ++writes;
    last_first = first;
    last_count = count;
    last_buffer = buffer;
    return outcome;
}

static pf_block_result_t sync_blocks(void *context)
{
    TEST_ASSERT_EQUAL_PTR(&media, context);
    ++syncs;
    return outcome;
}

static pf_block_result_t trim_blocks(void *context, uint32_t first, uint32_t count)
{
    return write_blocks(context, first, NULL, count);
}

static const pf_block_device_ops_t ops = {
    .geometry = geometry, .read = read_blocks, .write = write_blocks, .sync = sync_blocks,
};
static const pf_block_device_t device = {.ops = &ops, .context = &media};

void setUp(void)
{
    media = (pf_block_geometry_t){.sector_count = 8u, .sector_size = 512u, .erase_sector_count = 1u};
    outcome = PF_BLOCK_OK;
    reads = writes = syncs = 0u;
}
void tearDown(void) {}

void test_block_device_forwards_unaligned_batches_and_sync_without_copying(void)
{
    uint8_t buffer[1537];
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_block_read(&device, 5u, buffer + 1u, 3u));
    TEST_ASSERT_EQUAL_UINT(5u, last_first);
    TEST_ASSERT_EQUAL_UINT(3u, last_count);
    TEST_ASSERT_EQUAL_PTR(buffer + 1u, last_buffer);
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_block_write(&device, 5u, buffer + 1u, 3u));
    TEST_ASSERT_EQUAL_UINT(1u, writes);
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_block_sync(&device));
    TEST_ASSERT_EQUAL_UINT(1u, syncs);
}

void test_optional_trim_validates_range_and_write_protection_before_dispatch(void)
{
    pf_block_device_ops_t trimming_ops = ops;
    trimming_ops.trim = trim_blocks;
    pf_block_device_t trimming = {.ops = &trimming_ops, .context = &media};
    TEST_ASSERT_EQUAL(PF_BLOCK_INVALID_ARGUMENT, pf_block_trim(&device, 0u, 1u));
    TEST_ASSERT_EQUAL(PF_BLOCK_INVALID_ARGUMENT, pf_block_trim(&trimming, 0u, 0u));
    TEST_ASSERT_EQUAL(PF_BLOCK_INVALID_ARGUMENT, pf_block_trim(&trimming, 7u, 2u));
    TEST_ASSERT_EQUAL(PF_BLOCK_INVALID_ARGUMENT, pf_block_trim(&trimming, UINT32_MAX, 2u));
    media.write_protected = true;
    TEST_ASSERT_EQUAL(PF_BLOCK_WRITE_PROTECTED, pf_block_trim(&trimming, 0u, 1u));
    TEST_ASSERT_EQUAL_UINT(0u, writes);
    media.write_protected = false;
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_block_trim(&trimming, 2u, 3u));
    TEST_ASSERT_EQUAL_UINT(2u, last_first);
    TEST_ASSERT_EQUAL_UINT(3u, last_count);
    outcome = PF_BLOCK_NOT_READY;
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_block_trim(&trimming, 0u, 1u));
}

void test_block_device_rejects_invalid_descriptors_geometry_and_overflowing_ranges(void)
{
    uint8_t buffer[512];
    pf_block_geometry_t value;
    pf_block_device_t invalid = {0};
    TEST_ASSERT_EQUAL(PF_BLOCK_INVALID_ARGUMENT, pf_block_geometry(NULL, &value));
    TEST_ASSERT_EQUAL(PF_BLOCK_INVALID_ARGUMENT, pf_block_geometry(&invalid, &value));
    TEST_ASSERT_EQUAL(PF_BLOCK_INVALID_ARGUMENT, pf_block_geometry(&device, NULL));
    TEST_ASSERT_EQUAL(PF_BLOCK_INVALID_ARGUMENT, pf_block_sync(&invalid));
    TEST_ASSERT_EQUAL(PF_BLOCK_INVALID_ARGUMENT, pf_block_read(&device, 0u, NULL, 1u));
    TEST_ASSERT_EQUAL(PF_BLOCK_INVALID_ARGUMENT, pf_block_read(&device, 0u, buffer, 0u));
    TEST_ASSERT_EQUAL(PF_BLOCK_INVALID_ARGUMENT, pf_block_read(&device, 7u, buffer, 2u));
    TEST_ASSERT_EQUAL(PF_BLOCK_INVALID_ARGUMENT, pf_block_write(&device, UINT32_MAX, buffer, 2u));
    media.sector_count = UINT32_MAX;
    TEST_ASSERT_EQUAL(PF_BLOCK_INVALID_ARGUMENT, pf_block_read(&device, UINT32_MAX - 1u, buffer, 3u));
    media.sector_count = 0u;
    TEST_ASSERT_EQUAL(PF_BLOCK_IO_ERROR, pf_block_geometry(&device, &value));
    media.sector_count = 8u;
    media.sector_size = 0u;
    TEST_ASSERT_EQUAL(PF_BLOCK_IO_ERROR, pf_block_geometry(&device, &value));
    media.sector_size = 512u;
    media.erase_sector_count = 0u;
    TEST_ASSERT_EQUAL(PF_BLOCK_IO_ERROR, pf_block_geometry(&device, &value));
    TEST_ASSERT_EQUAL_UINT(0u, reads + writes);
}

void test_block_device_preserves_not_ready_write_protection_and_sync_errors(void)
{
    uint8_t buffer[512];
    outcome = PF_BLOCK_NOT_READY;
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_block_read(&device, 0u, buffer, 1u));
    TEST_ASSERT_EQUAL_UINT(0u, reads);
    outcome = PF_BLOCK_OK;
    media.write_protected = true;
    TEST_ASSERT_EQUAL(PF_BLOCK_WRITE_PROTECTED, pf_block_write(&device, 0u, buffer, 1u));
    TEST_ASSERT_EQUAL_UINT(0u, writes);
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_block_read(&device, 0u, buffer, 1u));
    outcome = PF_BLOCK_IO_ERROR;
    TEST_ASSERT_EQUAL(PF_BLOCK_IO_ERROR, pf_block_sync(&device));
    TEST_ASSERT_EQUAL_UINT(1u, syncs);
}

void test_block_device_rejects_each_missing_callback_before_dispatch(void)
{
    const pf_block_device_ops_t incomplete[] = {
        {.read = read_blocks, .write = write_blocks, .sync = sync_blocks},
        {.geometry = geometry, .write = write_blocks, .sync = sync_blocks},
        {.geometry = geometry, .read = read_blocks, .sync = sync_blocks},
        {.geometry = geometry, .read = read_blocks, .write = write_blocks},
    };
    uint8_t buffer[512];
    pf_block_geometry_t value;
    for (unsigned i = 0u; i < sizeof(incomplete) / sizeof(incomplete[0]); ++i) {
        const pf_block_device_t invalid = {.ops = &incomplete[i], .context = &media};
        TEST_ASSERT_EQUAL(PF_BLOCK_INVALID_ARGUMENT, pf_block_geometry(&invalid, &value));
        TEST_ASSERT_EQUAL(PF_BLOCK_INVALID_ARGUMENT, pf_block_read(&invalid, 0u, buffer, 1u));
        TEST_ASSERT_EQUAL(PF_BLOCK_INVALID_ARGUMENT, pf_block_write(&invalid, 0u, buffer, 1u));
        TEST_ASSERT_EQUAL(PF_BLOCK_INVALID_ARGUMENT, pf_block_sync(&invalid));
    }
    TEST_ASSERT_EQUAL_UINT(0u, reads + writes + syncs);
}

void test_block_device_rejects_byte_count_overflow_on_32_bit_hosts(void)
{
#if SIZE_MAX == UINT32_MAX
    uint8_t buffer[1];
    media.sector_count = UINT32_MAX;
    const uint32_t largest_count = SIZE_MAX / media.sector_size;

    /* These fake callbacks only record dispatch; no multi-gigabyte buffer is accessed. */
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_block_read(&device, 0u, buffer, largest_count));
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_block_write(&device, 0u, buffer, largest_count));
    TEST_ASSERT_EQUAL(PF_BLOCK_INVALID_ARGUMENT, pf_block_read(&device, 0u, buffer, largest_count + 1u));
    TEST_ASSERT_EQUAL(PF_BLOCK_INVALID_ARGUMENT, pf_block_write(&device, 0u, buffer, largest_count + 1u));
    TEST_ASSERT_EQUAL_UINT(1u, reads);
    TEST_ASSERT_EQUAL_UINT(1u, writes);
#else
    TEST_IGNORE_MESSAGE("Requires 32-bit size_t; use the documented block-device -m32 test command.");
#endif
}
