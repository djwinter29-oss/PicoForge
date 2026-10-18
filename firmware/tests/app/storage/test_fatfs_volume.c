#include "unity.h"
#include "fatfs_volume.h"
#include "Mockff.h"
#include "Mockfatfs_disk.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

static const pf_block_device_t device = {0};
static FRESULT mount_result;
static bool mounted_callback;
static unsigned unmount_calls;
static atomic_bool stop_reader;
static atomic_uint snapshots;

static void *read_volume_telemetry(void *context)
{
    (void)context;
    while (!atomic_load(&stop_reader)) {
        (void)pf_fatfs_volume_is_mounted();
        pf_fatfs_volume_filesystem_format_t format = pf_fatfs_volume_filesystem_format();
        assert(format >= PF_FATFS_VOLUME_FILESYSTEM_UNKNOWN && format <= PF_FATFS_VOLUME_FILESYSTEM_EXFAT);
        atomic_fetch_add(&snapshots, 1u);
    }
    return NULL;
}

static FRESULT mount_filesystem(FATFS *filesystem, const TCHAR *path, BYTE immediate, int calls)
{
    (void)calls;
    TEST_ASSERT_EQUAL_STRING(PF_FATFS_VOLUME_PATH, path);
    if (filesystem == NULL) {
        TEST_ASSERT_EQUAL_UINT(0u, immediate);
        ++unmount_calls;
        return mount_result;
    }
    TEST_ASSERT_FALSE(pf_fatfs_volume_is_mounted());
    TEST_ASSERT_EQUAL_UINT(1u, immediate);
    filesystem->fs_type = FS_EXFAT;
    mounted_callback = true;
    return mount_result;
}

void setUp(void)
{
    mount_result = FR_OK;
    mounted_callback = false;
    unmount_calls = 0u;
    f_mount_Stub(mount_filesystem);
}

void tearDown(void)
{
    if (pf_fatfs_volume_is_mounted()) {
        mount_result = FR_OK;
        pf_fatfs_disk_bind_Expect(NULL);
        TEST_ASSERT_EQUAL(PF_FATFS_VOLUME_OK, pf_fatfs_volume_unmount(NULL));
    }
}

void test_mount_binds_supplied_device_and_unmount_releases_binding(void)
{
    pf_fatfs_disk_bind_Expect(&device);
    FRESULT result;
    TEST_ASSERT_EQUAL(PF_FATFS_VOLUME_OK, pf_fatfs_volume_mount(&device, &result));
    TEST_ASSERT_TRUE(mounted_callback);
    TEST_ASSERT_TRUE(pf_fatfs_volume_is_mounted());
    TEST_ASSERT_EQUAL(PF_FATFS_VOLUME_FILESYSTEM_EXFAT, pf_fatfs_volume_filesystem_format());
    TEST_ASSERT_EQUAL(PF_FATFS_VOLUME_ALREADY_MOUNTED, pf_fatfs_volume_mount(&device, &result));
    pf_fatfs_disk_bind_Expect(NULL);
    TEST_ASSERT_EQUAL(PF_FATFS_VOLUME_OK, pf_fatfs_volume_unmount(&result));
    TEST_ASSERT_EQUAL_UINT(1u, unmount_calls);
    TEST_ASSERT_FALSE(pf_fatfs_volume_is_mounted());
    TEST_ASSERT_EQUAL(PF_FATFS_VOLUME_FILESYSTEM_UNKNOWN, pf_fatfs_volume_filesystem_format());
}

void test_failed_mount_unbinds_device_and_reports_filesystem_error(void)
{
    pf_fatfs_disk_bind_Expect(&device);
    pf_fatfs_disk_bind_Expect(NULL);
    mount_result = FR_DISK_ERR;
    FRESULT result;
    TEST_ASSERT_EQUAL(PF_FATFS_VOLUME_FILESYSTEM_ERROR, pf_fatfs_volume_mount(&device, &result));
    TEST_ASSERT_EQUAL(FR_DISK_ERR, result);
    TEST_ASSERT_EQUAL_UINT(1u, unmount_calls);
    TEST_ASSERT_FALSE(pf_fatfs_volume_is_mounted());
}

void test_missing_device_never_binds_or_mounts(void)
{
    TEST_ASSERT_EQUAL(PF_FATFS_VOLUME_INVALID_ARGUMENT, pf_fatfs_volume_mount(NULL, NULL));
    TEST_ASSERT_FALSE(mounted_callback);
}

void test_failed_unmount_retains_mounted_state_and_binding(void)
{
    pf_fatfs_disk_bind_Expect(&device);
    TEST_ASSERT_EQUAL(PF_FATFS_VOLUME_OK, pf_fatfs_volume_mount(&device, NULL));
    mount_result = FR_DISK_ERR;
    TEST_ASSERT_EQUAL(PF_FATFS_VOLUME_FILESYSTEM_ERROR, pf_fatfs_volume_unmount(NULL));
    TEST_ASSERT_TRUE(pf_fatfs_volume_is_mounted());
}

void test_format_invalidation_does_not_unregister_mounted_volume(void)
{
    pf_fatfs_disk_bind_Expect(&device);
    TEST_ASSERT_EQUAL(PF_FATFS_VOLUME_OK, pf_fatfs_volume_mount(&device, NULL));
    pf_fatfs_volume_forget_filesystem_format();
    TEST_ASSERT_TRUE(pf_fatfs_volume_is_mounted());
    TEST_ASSERT_EQUAL(PF_FATFS_VOLUME_FILESYSTEM_UNKNOWN, pf_fatfs_volume_filesystem_format());
}

void test_telemetry_can_be_read_during_serialized_mount_and_unmount(void)
{
    atomic_store(&stop_reader, false);
    atomic_store(&snapshots, 0u);
    pf_fatfs_disk_bind_Ignore();
    pthread_t reader;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&reader, NULL, read_volume_telemetry, NULL));
    while (atomic_load(&snapshots) == 0u) {
        sched_yield();
    }
    for (unsigned i = 0; i < 1000u; ++i) {
        assert(pf_fatfs_volume_mount(&device, NULL) == PF_FATFS_VOLUME_OK);
        assert(pf_fatfs_volume_unmount(NULL) == PF_FATFS_VOLUME_OK);
    }
    atomic_store(&stop_reader, true);
    TEST_ASSERT_EQUAL_INT(0, pthread_join(reader, NULL));
    TEST_ASSERT_GREATER_THAN_UINT(0u, atomic_load(&snapshots));
    TEST_ASSERT_FALSE(pf_fatfs_volume_is_mounted());
}
