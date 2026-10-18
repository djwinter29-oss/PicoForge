#include "unity.h"
#include "Mocklogging.h"
#include "Mockff.h"
#include "storage_commands.h"
#include "embedded_cli.h"
TEST_INCLUDE_PATH("firmware/tests/support/storage")
#define WORKER_SOURCE "../../../src/app/storage/storage_worker.c"
#include WORKER_SOURCE

static bool nor_ready, usb_owned, remount, mounted, init_fails;
static unsigned depth, init_calls, mount_calls, unmount_calls;
static uint32_t now;
static const pf_block_device_t device = {0};
static pf_fatfs_volume_result_t mount_result;
static FRESULT mount_reason;
static storage_request_t queued_request;
static pf_storage_worker_reply_t queued_replies[8];
static unsigned reply_count, reply_index;

void pf_nor_media_lock(void) { ++depth; }
void pf_nor_media_unlock(void) { TEST_ASSERT_GREATER_THAN_UINT(0u, depth); --depth; }
bool pf_nor_media_usb_owned(void) { return usb_owned; }
bool pf_nor_media_ready(void) { return nor_ready; }
bool pf_nor_media_take_remount_request(void)
{
    bool value = remount;
    remount = false;
    return value;
}
pf_block_result_t pf_nor_media_initialize(void)
{
    TEST_ASSERT_FALSE(usb_owned || mounted);
    ++init_calls;
    nor_ready = !init_fails;
    return nor_ready ? PF_BLOCK_OK : PF_BLOCK_IO_ERROR;
}
const pf_block_device_t *pf_nor_media_firmware_device(void)
{
    return &device;
}
bool pf_fatfs_volume_is_mounted(void) { return mounted; }
pf_fatfs_volume_result_t pf_fatfs_volume_mount(const pf_block_device_t *value, FRESULT *result)
{
    if (result != NULL) {
        *result = mount_reason;
    }
    TEST_ASSERT_EQUAL_PTR(&device, value);
    TEST_ASSERT_TRUE(nor_ready && !usb_owned);
    ++mount_calls;
    mounted = mount_result == PF_FATFS_VOLUME_OK;
    return mount_result;
}
pf_fatfs_volume_result_t pf_fatfs_volume_unmount(FRESULT *result)
{
    (void)result;
    ++unmount_calls;
    mounted = false;
    return PF_FATFS_VOLUME_OK;
}
uint32_t pf_system_uptime_ms(void) { return now; }

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout)
{
    (void)timeout;
    if (queue == requests) {
        queued_request = *(const storage_request_t *)item;
    } else {
        TEST_ASSERT_EQUAL_PTR(replies, queue);
        TEST_ASSERT_LESS_THAN_UINT(8u, reply_count);
        queued_replies[reply_count++] = *(const pf_storage_worker_reply_t *)item;
    }
    return pdPASS;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout)
{
    (void)timeout;
    TEST_ASSERT_EQUAL_PTR(replies, queue);
    if (reply_index == reply_count) {
        return 0;
    }
    *(pf_storage_worker_reply_t *)item = queued_replies[reply_index++];
    return pdPASS;
}

void setUp(void)
{
    pf_log_write_Ignore();
    nor_ready = usb_owned = remount = mounted = init_fails = false;
    busy = media_service_busy = auto_mount_blocked = mount_retry_waiting = false;
    auto_mount_pending = true;
    mount_failure_reported = false;
    mount_result = PF_FATFS_VOLUME_OK;
    mount_reason = FR_OK;
    requests = (void *)1;
    replies = (void *)2;
    reply_count = reply_index = 0u;
    cancelled = false;
    benchmark_active = benchmark_finished = false;
    depth = init_calls = mount_calls = unmount_calls = now = 0u;
}
void tearDown(void)
{
    TEST_ASSERT_EQUAL_UINT(0u, depth);
    TEST_ASSERT_FALSE(media_service_busy);
}

void test_nor_automount_retries_initialization_and_excludes_usb_and_active_worker(void)
{
    init_fails = true;
    service_media();
    TEST_ASSERT_EQUAL_UINT(1u, init_calls);
    TEST_ASSERT_EQUAL_UINT(0u, mount_calls);
    init_fails = false;
    now = MEDIA_MOUNT_RETRY_MS - 1u;
    service_media();
    TEST_ASSERT_EQUAL_UINT(1u, init_calls);
    now++;
    usb_owned = true;
    service_media();
    TEST_ASSERT_EQUAL_UINT(1u, init_calls);
    usb_owned = false;
    busy = true;
    service_media();
    TEST_ASSERT_EQUAL_UINT(1u, init_calls);
    busy = false;
    service_media();
    TEST_ASSERT_TRUE(mounted && !auto_mount_pending);
    TEST_ASSERT_EQUAL_UINT(1u, mount_calls);
}

void test_usb_release_requests_remount_and_fault_recovery_never_flushes_invalid_media(void)
{
    service_media();
    mounted = false;
    auto_mount_pending = false;
    usb_owned = true;
    service_media();
    TEST_ASSERT_EQUAL_UINT(1u, mount_calls);
    usb_owned = false;
    remount = true;
    service_media();
    TEST_ASSERT_EQUAL_UINT(2u, mount_calls);
    nor_ready = false;
    service_media();
    TEST_ASSERT_EQUAL_UINT(1u, unmount_calls);
    TEST_ASSERT_EQUAL_UINT(3u, mount_calls);
    mounted = false;
    busy = true;
    recover_media();
    TEST_ASSERT_TRUE(nor_ready && auto_mount_blocked && !busy && !mounted);
    service_media();
    TEST_ASSERT_EQUAL_UINT(3u, mount_calls);
}

void test_automount_reports_first_and_changed_failure_but_not_identical_retries(void)
{
    pf_log_write_StopIgnore();
    pf_log_write_ExpectAnyArgs();
    mount_result = PF_FATFS_VOLUME_FILESYSTEM_ERROR;
    mount_reason = FR_NO_FILESYSTEM;
    service_media();
    TEST_ASSERT_TRUE(mount_failure_reported && auto_mount_pending);
    TEST_ASSERT_EQUAL(FR_NO_FILESYSTEM, last_filesystem_result);
    now += MEDIA_MOUNT_RETRY_MS;
    service_media();
    mount_reason = FR_DISK_ERR;
    pf_log_write_ExpectAnyArgs();
    now += MEDIA_MOUNT_RETRY_MS;
    service_media();
    TEST_ASSERT_EQUAL(FR_DISK_ERR, last_filesystem_result);
    mount_result = PF_FATFS_VOLUME_OK;
    mount_reason = FR_OK;
    now += MEDIA_MOUNT_RETRY_MS;
    service_media();
    TEST_ASSERT_FALSE(mount_failure_reported);
    mounted = false;
    auto_mount_pending = true;
    mount_result = PF_FATFS_VOLUME_FILESYSTEM_ERROR;
    mount_reason = FR_DISK_ERR;
    pf_log_write_ExpectAnyArgs();
    service_media();
}

void test_explicit_unmount_stays_unmounted_and_reply_drain_holds_admission(void)
{
    service_media();
    TEST_ASSERT_TRUE(mounted);
    TEST_ASSERT_TRUE(pf_storage_worker_submit("fs", "unmount", false, 42u));
    execute_command(&queued_request);
    TEST_ASSERT_FALSE(mounted || auto_mount_pending);
    TEST_ASSERT_TRUE(pf_storage_worker_busy());
    TEST_ASSERT_FALSE(pf_storage_worker_submit("fs", "mount", false, 42u));
    service_media();
    TEST_ASSERT_FALSE(mounted);
    pf_storage_worker_reply_t reply;
    TEST_ASSERT_TRUE(pf_storage_worker_poll(&reply));
    TEST_ASSERT_FALSE(reply.done);
    TEST_ASSERT_TRUE(pf_storage_worker_busy());
    TEST_ASSERT_TRUE(pf_storage_worker_poll(&reply));
    TEST_ASSERT_TRUE(reply.done);
    TEST_ASSERT_FALSE(pf_storage_worker_busy());
    now += MEDIA_MOUNT_RETRY_MS;
    service_media();
    TEST_ASSERT_FALSE(mounted);
}

void test_explicit_mount_after_recovery_restores_background_policy(void)
{
    config_available = true;
    recover_media();
    TEST_ASSERT_TRUE(auto_mount_blocked);
    TEST_ASSERT_TRUE(pf_storage_worker_submit("fs", "mount", false, 7u));
    execute_command(&queued_request);
    TEST_ASSERT_TRUE(mounted);
    TEST_ASSERT_FALSE(auto_mount_blocked);
    pf_storage_worker_reply_t reply;
    while (pf_storage_worker_poll(&reply)) {}
    nor_ready = false;
    service_media();
    TEST_ASSERT_TRUE(mounted && nor_ready);
}
