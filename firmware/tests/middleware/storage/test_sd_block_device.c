#include "unity.h"
#include "sd_block_device.h"
#include "block_device.h"
#include "sd_block.h"

static bool ready;
static pf_sd_owner_t owner;
static unsigned depth, calls;
static pf_sd_result_t result;
static const void *last_buffer;
static uint32_t last_first, last_count;

void pf_sd_lock(void) { ++depth; }
void pf_sd_unlock(void) { TEST_ASSERT_EQUAL_UINT(1u, depth); --depth; }
bool pf_sd_is_initialized(void) { TEST_ASSERT_EQUAL_UINT(1u, depth); return ready; }
uint32_t pf_sd_block_count(void) { TEST_ASSERT_EQUAL_UINT(1u, depth); return 8u; }
pf_sd_result_t pf_sd_read_blocks(uint32_t first, void *buffer, uint32_t count)
{
    ++calls;
    last_first = first;
    last_count = count;
    last_buffer = buffer;
    return owner == PF_SD_OWNER_FIRMWARE ? result : PF_SD_NOT_READY;
}
pf_sd_result_t pf_sd_write_blocks(uint32_t first, const void *buffer, uint32_t count)
{
    ++calls;
    last_first = first;
    last_count = count;
    last_buffer = buffer;
    return owner == PF_SD_OWNER_FIRMWARE ? result : PF_SD_NOT_READY;
}
pf_sd_result_t pf_sd_sync(void) { ++calls; return owner == PF_SD_OWNER_FIRMWARE ? result : PF_SD_NOT_READY; }

void setUp(void)
{
    ready = true;
    owner = PF_SD_OWNER_FIRMWARE;
    depth = calls = 0u;
    result = PF_SD_OK;
}
void tearDown(void) { TEST_ASSERT_EQUAL_UINT(0u, depth); }

void test_sd_device_snapshots_geometry_under_existing_reservation_and_preserves_owner_checks(void)
{
    pf_block_geometry_t geometry;
    const pf_block_device_t *device = pf_storage_sd_block_device();
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_block_geometry(device, &geometry));
    TEST_ASSERT_EQUAL_UINT(8u, geometry.sector_count);
    TEST_ASSERT_EQUAL_UINT(512u, geometry.sector_size);
    TEST_ASSERT_EQUAL_UINT(1u, geometry.erase_sector_count);
    TEST_ASSERT_FALSE(geometry.write_protected);
    owner = PF_SD_OWNER_USB_MSC;
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_block_geometry(device, &geometry));
    uint8_t buffer[512];
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_block_read(device, 0u, buffer, 1u));
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_block_write(device, 0u, buffer, 1u));
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_block_sync(device));
    TEST_ASSERT_EQUAL_UINT(3u, calls);
    owner = PF_SD_OWNER_FIRMWARE;
    ready = false;
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_block_geometry(device, &geometry));
}

void test_sd_device_maps_results_and_forwards_each_request_exactly_once(void)
{
    uint8_t buffer[1024];
    const pf_block_device_t *device = pf_storage_sd_block_device();
    const pf_sd_result_t sd_results[] = {
        PF_SD_OK, PF_SD_INVALID_ARGUMENT, PF_SD_NOT_CONFIGURED, PF_SD_NOT_READY,
        PF_SD_IO_ERROR, PF_SD_INIT_FAILED, PF_SD_CONFIG_LOCKED,
    };
    const pf_block_result_t expected[] = {
        PF_BLOCK_OK, PF_BLOCK_INVALID_ARGUMENT, PF_BLOCK_NOT_READY, PF_BLOCK_NOT_READY,
        PF_BLOCK_IO_ERROR, PF_BLOCK_IO_ERROR, PF_BLOCK_IO_ERROR,
    };
    for (unsigned i = 0u; i < sizeof(sd_results) / sizeof(sd_results[0]); ++i) {
        result = sd_results[i];
        TEST_ASSERT_EQUAL(expected[i], pf_block_read(device, 2u, buffer, 2u));
        TEST_ASSERT_EQUAL(expected[i], pf_block_write(device, 2u, buffer, 2u));
        TEST_ASSERT_EQUAL(expected[i], pf_block_sync(device));
        TEST_ASSERT_EQUAL_UINT((i + 1u) * 3u, calls);
        TEST_ASSERT_EQUAL_UINT(2u, last_first);
        TEST_ASSERT_EQUAL_UINT(2u, last_count);
        TEST_ASSERT_EQUAL_PTR(buffer, last_buffer);
    }
}
