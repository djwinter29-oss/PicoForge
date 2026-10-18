#include "unity.h"
#include "fatfs_volume.h"
#include "Mocksd.h"
#include "Mockff.h"
#include "Mockfatfs_disk.h"
#include "Mocksd_block_device.h"

static pf_sd_config_t config;
static const pf_block_device_t device = {0};
static FRESULT mount_result;
static bool mounted_callback;
static unsigned unmount_calls;

static FRESULT mount_filesystem(FATFS *filesystem, const TCHAR *path, BYTE immediate, int calls)
{
    (void)calls;
    TEST_ASSERT_EQUAL_STRING(PF_STORAGE_VOLUME, path);
    if (filesystem == NULL) {
        TEST_ASSERT_EQUAL_UINT(0u, immediate);
        ++unmount_calls;
        return mount_result;
    }
    TEST_ASSERT_EQUAL_UINT(1u, immediate);
    filesystem->fs_type = FS_EXFAT;
    mounted_callback = true;
    return mount_result;
}

void setUp(void)
{
    config = (pf_sd_config_t){0};
    mount_result = FR_OK;
    mounted_callback = false;
    unmount_calls = 0u;
    f_mount_Stub(mount_filesystem);
}

void tearDown(void)
{
    if (pf_storage_is_mounted()) {
        mount_result = FR_OK;
        pf_fatfs_bind_device_Expect(NULL);
        TEST_ASSERT_EQUAL(PF_STORAGE_OK, pf_storage_unmount(NULL));
    }
}

void test_mount_binds_sd_device_and_unmount_releases_binding(void)
{
    pf_sd_init_ExpectAndReturn(&config, PF_SD_OK);
    pf_storage_sd_block_device_ExpectAndReturn(&device);
    pf_fatfs_bind_device_Expect(&device);
    FRESULT result;
    TEST_ASSERT_EQUAL(PF_STORAGE_OK, pf_storage_mount(&config, &result));
    TEST_ASSERT_TRUE(mounted_callback);
    TEST_ASSERT_TRUE(pf_storage_is_mounted());
    TEST_ASSERT_EQUAL(PF_STORAGE_FILESYSTEM_EXFAT, pf_storage_filesystem_format());
    TEST_ASSERT_EQUAL(PF_STORAGE_ALREADY_MOUNTED, pf_storage_mount(&config, &result));
    pf_fatfs_bind_device_Expect(NULL);
    TEST_ASSERT_EQUAL(PF_STORAGE_OK, pf_storage_unmount(&result));
    TEST_ASSERT_EQUAL_UINT(1u, unmount_calls);
    TEST_ASSERT_FALSE(pf_storage_is_mounted());
    TEST_ASSERT_EQUAL(PF_STORAGE_FILESYSTEM_UNKNOWN, pf_storage_filesystem_format());
}

void test_failed_mount_unbinds_device_and_reports_filesystem_error(void)
{
    pf_sd_init_ExpectAndReturn(&config, PF_SD_OK);
    pf_storage_sd_block_device_ExpectAndReturn(&device);
    pf_fatfs_bind_device_Expect(&device);
    pf_fatfs_bind_device_Expect(NULL);
    mount_result = FR_DISK_ERR;
    FRESULT result;
    TEST_ASSERT_EQUAL(PF_STORAGE_FILESYSTEM_ERROR, pf_storage_mount(&config, &result));
    TEST_ASSERT_EQUAL(FR_DISK_ERR, result);
    TEST_ASSERT_EQUAL_UINT(1u, unmount_calls);
    TEST_ASSERT_FALSE(pf_storage_is_mounted());
}

void test_failed_card_initialization_never_binds_or_mounts(void)
{
    TEST_ASSERT_EQUAL(PF_STORAGE_INVALID_ARGUMENT, pf_storage_mount(NULL, NULL));
    pf_sd_init_ExpectAndReturn(&config, PF_SD_INIT_FAILED);
    TEST_ASSERT_EQUAL(PF_STORAGE_SD_CARD_ERROR, pf_storage_mount(&config, NULL));
    TEST_ASSERT_FALSE(mounted_callback);
}

void test_failed_unmount_retains_mounted_state_and_binding(void)
{
    pf_sd_init_ExpectAndReturn(&config, PF_SD_OK);
    pf_storage_sd_block_device_ExpectAndReturn(&device);
    pf_fatfs_bind_device_Expect(&device);
    TEST_ASSERT_EQUAL(PF_STORAGE_OK, pf_storage_mount(&config, NULL));
    mount_result = FR_DISK_ERR;
    TEST_ASSERT_EQUAL(PF_STORAGE_FILESYSTEM_ERROR, pf_storage_unmount(NULL));
    TEST_ASSERT_TRUE(pf_storage_is_mounted());
}
