#include "unity.h"

TEST_INCLUDE_PATH("firmware/src/integration/levelx")
TEST_INCLUDE_PATH("firmware/third_party/levelx/common/inc")

#include "levelx_nor.h"
#include "middleware/storage/levelx_block_device.h"
#include "middleware/storage/block_device.h"
#include "middleware/storage/fatfs_disk.h"
#include "ff.h"

/* Scope the real filesystem engine to this integration test, not all tests including ff.h. */
#define FATFS_SOURCE "../../../third_party/fatfs/source/ff.c"
#define FATFS_UNICODE_SOURCE "../../../third_party/fatfs/source/ffunicode.c"
#include FATFS_SOURCE
#include FATFS_UNICODE_SOURCE

TEST_SOURCE_FILE("lx_nor_flash_initialize.c")
TEST_SOURCE_FILE("lx_nor_flash_open_extended.c")
TEST_SOURCE_FILE("lx_nor_flash_close.c")
TEST_SOURCE_FILE("lx_nor_flash_driver_read.c")
TEST_SOURCE_FILE("lx_nor_flash_driver_write.c")
TEST_SOURCE_FILE("lx_nor_flash_driver_block_erase.c")
TEST_SOURCE_FILE("lx_nor_flash_system_error.c")
TEST_SOURCE_FILE("lx_nor_flash_sector_read.c")
TEST_SOURCE_FILE("lx_nor_flash_sector_write.c")
TEST_SOURCE_FILE("lx_nor_flash_logical_sector_find.c")
TEST_SOURCE_FILE("lx_nor_flash_physical_sector_allocate.c")
TEST_SOURCE_FILE("lx_nor_flash_sector_mapping_cache_invalidate.c")
TEST_SOURCE_FILE("lx_nor_flash_block_reclaim.c")
TEST_SOURCE_FILE("lx_nor_flash_next_block_to_erase_find.c")
TEST_SOURCE_FILE("lx_nor_flash_sector_release.c")
TEST_SOURCE_FILE("lx_nor_flash_partial_defragment.c")

#include <string.h>

#define BLOCKS 14u
static uint8_t memory[BLOCKS * 4096u];
static pf_nor_t flash;
static pf_levelx_nor_t volume;
static unsigned erases[BLOCKS], programs;
static bool fail_io;
static int fail_program_after;

pf_nor_result_t pf_nor_read(pf_nor_t *device, uint32_t address, void *data, size_t length)
{
    (void)device;
    if (fail_io) return PF_NOR_IO_ERROR;
    TEST_ASSERT_TRUE(address <= sizeof(memory) && length <= sizeof(memory) - address);
    memcpy(data, memory + address, length);
    return PF_NOR_OK;
}

pf_nor_result_t pf_nor_program(pf_nor_t *device, uint32_t address,
                                    const void *data, size_t length)
{
    (void)device;
    if (fail_io) return PF_NOR_IO_ERROR;
    TEST_ASSERT_TRUE(address <= sizeof(memory) && length <= sizeof(memory) - address);
    const uint8_t *bytes = data;
    bool cut = fail_program_after == 0;
    if (fail_program_after >= 0) fail_program_after--;
    size_t written = cut ? length / 2u : length;
    for (size_t index = 0u; index < written; index++) {
        TEST_ASSERT_EQUAL_HEX8(bytes[index], memory[address + index] & bytes[index]);
        memory[address + index] &= bytes[index];
    }
    programs++;
    if (cut) {
        fail_io = true;
        return PF_NOR_IO_ERROR;
    }
    return PF_NOR_OK;
}

pf_nor_result_t pf_nor_erase(pf_nor_t *device, uint32_t address)
{
    (void)device;
    if (fail_io) return PF_NOR_IO_ERROR;
    TEST_ASSERT_EQUAL_UINT32(0u, address % 4096u);
    TEST_ASSERT_TRUE(address < sizeof(memory));
    memset(memory + address, 0xff, 4096u);
    erases[address / 4096u]++;
    return PF_NOR_OK;
}

pf_nor_result_t pf_nor_sync(pf_nor_t *device)
{
    (void)device;
    return fail_io ? PF_NOR_IO_ERROR : PF_NOR_OK;
}

void setUp(void)
{
    memset(memory, 0xff, sizeof(memory));
    memset(erases, 0, sizeof(erases));
    memset(&volume, 0, sizeof(volume));
    flash = (pf_nor_t){
        .initialized = true, .capacity_bytes = sizeof(memory),
        .page_size = 256u, .erase_size = 4096u,
    };
    fail_io = false;
    fail_program_after = -1;
    programs = 0u;
}

void tearDown(void)
{
    fail_io = false;
    if (volume.opened) {
        TEST_ASSERT_EQUAL(LX_SUCCESS, pf_levelx_nor_close(&volume));
    }
}

static void open_volume(void)
{
    TEST_ASSERT_EQUAL(LX_SUCCESS, pf_levelx_nor_open(&volume, &flash, 4096u, (BLOCKS - 2u) * 4096u));
    TEST_ASSERT_EQUAL_UINT32(70u, volume.sector_count);
}

void test_real_levelx_rewrites_and_reopens_with_unaligned_buffers(void)
{
    open_volume();
    uint8_t input[513], output[513];
    memset(input, 0x12, sizeof(input));
    TEST_ASSERT_EQUAL(LX_SUCCESS, pf_levelx_nor_write(&volume, 0u, input + 1u));
    memset(input, 0xff, sizeof(input));
    TEST_ASSERT_EQUAL(LX_SUCCESS, pf_levelx_nor_write(&volume, 0u, input + 1u));
    TEST_ASSERT_EQUAL(LX_SUCCESS, pf_levelx_nor_sync(&volume));
    TEST_ASSERT_EQUAL(LX_SUCCESS, pf_levelx_nor_close(&volume));
    open_volume();
    TEST_ASSERT_EQUAL(LX_SUCCESS, pf_levelx_nor_read(&volume, 0u, output + 1u));
    TEST_ASSERT_EQUAL_MEMORY(input + 1u, output + 1u, 512u);
}

void test_reclaim_preserves_cold_data_and_stays_inside_region(void)
{
    open_volume();
    uint8_t input[512], output[512];
    for (uint32_t sector = 0u; sector < volume.sector_count; sector++) {
        memset(input, (int)sector, sizeof(input));
        TEST_ASSERT_EQUAL(LX_SUCCESS, pf_levelx_nor_write(&volume, sector, input));
    }
    for (unsigned iteration = 0u; iteration < 200u; iteration++) {
        memset(input, (int)iteration, sizeof(input));
        TEST_ASSERT_EQUAL(LX_SUCCESS, pf_levelx_nor_write(&volume, 0u, input));
    }
    TEST_ASSERT_EQUAL(LX_SUCCESS, pf_levelx_nor_close(&volume));
    open_volume();
    for (uint32_t sector = 0u; sector < volume.sector_count; sector++) {
        memset(input, sector == 0u ? 199 : (int)sector, sizeof(input));
        TEST_ASSERT_EQUAL(LX_SUCCESS, pf_levelx_nor_read(&volume, sector, output));
        TEST_ASSERT_EQUAL_MEMORY(input, output, sizeof(input));
    }
    unsigned reclaimed = 0u;
    for (unsigned block = 1u; block < BLOCKS - 1u; block++) reclaimed += erases[block];
    TEST_ASSERT_GREATER_THAN_UINT32(0u, reclaimed);
    TEST_ASSERT_EQUAL_UINT32(0u, erases[0]);
    TEST_ASSERT_EQUAL_UINT32(0u, erases[BLOCKS - 1u]);
    for (unsigned index = 0u; index < 4096u; index++) {
        TEST_ASSERT_EQUAL_HEX8(0xffu, memory[index]);
        TEST_ASSERT_EQUAL_HEX8(0xffu, memory[(BLOCKS - 1u) * 4096u + index]);
    }
}

void test_ranges_and_geometry_rejected_without_io(void)
{
    TEST_ASSERT_NOT_EQUAL(LX_SUCCESS, pf_levelx_nor_open(&volume, &flash, 1u, 4096u));
    TEST_ASSERT_EQUAL_UINT32(0u, programs);
    open_volume();
    uint8_t data[512];
    unsigned before = programs;
    TEST_ASSERT_NOT_EQUAL(LX_SUCCESS, pf_levelx_nor_write(&volume, volume.sector_count, data));
    TEST_ASSERT_NOT_EQUAL(LX_SUCCESS, pf_levelx_nor_read(&volume, 0u, NULL));
    TEST_ASSERT_EQUAL_UINT32(before, programs);
}

void test_media_fault_latches_and_requires_close_and_recovery(void)
{
    open_volume();
    uint8_t data[512] = {0};
    fail_io = true;
    TEST_ASSERT_NOT_EQUAL(LX_SUCCESS, pf_levelx_nor_write(&volume, 0u, data));
    TEST_ASSERT_TRUE(volume.faulted);
    TEST_ASSERT_EQUAL(PF_NOR_IO_ERROR, volume.last_raw_result);
    fail_io = false;
    TEST_ASSERT_NOT_EQUAL(LX_SUCCESS, pf_levelx_nor_write(&volume, 0u, data));
    TEST_ASSERT_EQUAL(LX_SUCCESS, pf_levelx_nor_close(&volume));
    open_volume();
}

void test_interrupted_sector_updates_recover_without_losing_cold_sector(void)
{
    static uint8_t snapshot[sizeof(memory)];
    uint8_t old[512], newer[512], cold[512], output[512];
    memset(old, 0x33, sizeof(old));
    memset(newer, 0x55, sizeof(newer));
    memset(cold, 0x77, sizeof(cold));
    open_volume();
    TEST_ASSERT_EQUAL(LX_SUCCESS, pf_levelx_nor_write(&volume, 0u, old));
    TEST_ASSERT_EQUAL(LX_SUCCESS, pf_levelx_nor_write(&volume, 1u, cold));
    TEST_ASSERT_EQUAL(LX_SUCCESS, pf_levelx_nor_close(&volume));
    memcpy(snapshot, memory, sizeof(memory));
    unsigned interrupted = 0u;
    for (int cut = 0; cut < 12; cut++) {
        memcpy(memory, snapshot, sizeof(memory));
        fail_io = false;
        fail_program_after = -1;
        open_volume();
        fail_program_after = cut;
        UINT result = pf_levelx_nor_write(&volume, 0u, newer);
        interrupted += result != LX_SUCCESS;
        fail_io = false;
        fail_program_after = -1;
        TEST_ASSERT_EQUAL(LX_SUCCESS, pf_levelx_nor_close(&volume));
        open_volume();
        TEST_ASSERT_EQUAL(LX_SUCCESS, pf_levelx_nor_read(&volume, 1u, output));
        TEST_ASSERT_EQUAL_MEMORY(cold, output, sizeof(cold));
        TEST_ASSERT_EQUAL(LX_SUCCESS, pf_levelx_nor_read(&volume, 0u, output));
        TEST_ASSERT_TRUE(memcmp(old, output, sizeof(old)) == 0 ||
                         memcmp(newer, output, sizeof(newer)) == 0);
        if (result == LX_SUCCESS) {
            TEST_ASSERT_EQUAL_MEMORY(newer, output, sizeof(newer));
        }
        TEST_ASSERT_EQUAL(LX_SUCCESS, pf_levelx_nor_close(&volume));
    }
    TEST_ASSERT_GREATER_THAN_UINT32(0u, interrupted);
}

void test_real_fatfs_creates_syncs_and_reopens_a_file_over_levelx(void)
{
    open_volume();
    /* Tiny FAT12 superfloppy exists only in this simulated, bounds-checked flash. */
    uint8_t sector[512] = {0};
    sector[0] = 0xebu; sector[1] = 0x3cu; sector[2] = 0x90u;
    memcpy(sector + 3u, "MSDOS5.0", 8u);
    sector[11] = 0u; sector[12] = 2u;
    sector[13] = 1u; sector[14] = 1u; sector[16] = 1u; sector[17] = 16u;
    sector[19] = (uint8_t)volume.sector_count; sector[21] = 0xf8u; sector[22] = 1u;
    memcpy(sector + 54u, "FAT12   ", 8u);
    sector[510] = 0x55u; sector[511] = 0xaau;
    TEST_ASSERT_EQUAL(LX_SUCCESS, pf_levelx_nor_write(&volume, 0u, sector));
    memset(sector, 0, sizeof(sector));
    sector[0] = 0xf8u; sector[1] = sector[2] = 0xffu;
    TEST_ASSERT_EQUAL(LX_SUCCESS, pf_levelx_nor_write(&volume, 1u, sector));
    memset(sector, 0, sizeof(sector));
    TEST_ASSERT_EQUAL(LX_SUCCESS, pf_levelx_nor_write(&volume, 2u, sector));

    pf_block_device_t device = pf_levelx_block_device(&volume);
    pf_fatfs_disk_bind(&device);
    FATFS filesystem;
    FIL file;
    UINT transferred;
    uint8_t input[700], output[700];
    memset(input, 0x5a, sizeof(input));
    TEST_ASSERT_EQUAL(FR_OK, f_mount(&filesystem, "0:", 1u));
    TEST_ASSERT_EQUAL(FS_FAT12, filesystem.fs_type);
    TEST_ASSERT_EQUAL(FR_OK, f_open(&file, "0:/NOR.BIN", FA_CREATE_NEW | FA_WRITE));
    TEST_ASSERT_EQUAL(FR_OK, f_write(&file, input, sizeof(input), &transferred));
    TEST_ASSERT_EQUAL_UINT(sizeof(input), transferred);
    TEST_ASSERT_EQUAL(FR_OK, f_sync(&file));
    TEST_ASSERT_EQUAL(FR_OK, f_close(&file));
    TEST_ASSERT_EQUAL(FR_OK, f_mount(NULL, "0:", 0u));
    pf_fatfs_disk_bind(NULL);
    TEST_ASSERT_EQUAL(LX_SUCCESS, pf_levelx_nor_close(&volume));
    open_volume();
    pf_fatfs_disk_bind(&device);
    TEST_ASSERT_EQUAL(FR_OK, f_mount(&filesystem, "0:", 1u));
    TEST_ASSERT_EQUAL(FR_OK, f_open(&file, "0:/NOR.BIN", FA_READ));
    TEST_ASSERT_EQUAL(FR_OK, f_read(&file, output, sizeof(output), &transferred));
    TEST_ASSERT_EQUAL_UINT(sizeof(output), transferred);
    TEST_ASSERT_EQUAL_MEMORY(input, output, sizeof(input));
    TEST_ASSERT_EQUAL(FR_OK, f_close(&file));
    TEST_ASSERT_EQUAL(FR_OK, f_unlink("0:/NOR.BIN"));
    TEST_ASSERT_EQUAL(FR_OK, f_mount(NULL, "0:", 0u));
    pf_fatfs_disk_bind(NULL);
}
