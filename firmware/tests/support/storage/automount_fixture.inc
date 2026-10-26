#include "unity.h"
#include <string.h>
#include "platform/storage/storage.h"
#include "drivers/sdcard/sd_port.h"
#include "drivers/sdcard/sd_backend.h"
#define WORKER_SOURCE "app/storage/storage_worker.c"
#define FACADE_SOURCE "drivers/sdcard/sd.c"
#define MSC_SOURCE "app/usb/usb_msc.c"
#include WORKER_SOURCE
#include FACADE_SOURCE
#define MEDIA_SOURCE "platform/storage/storage.c"
#include MEDIA_SOURCE
#include MSC_SOURCE

uint64_t test_time_us;
static unsigned reservation_depth, try_lock_calls;
static bool lock_unavailable;
static pf_sd_info_t media = { .presence_known = true, .present = true };
static uint32_t now;
static bool mounted, queue_fails, queued_recovery, queued_unmount, sync_ok = true, init_fails;
static unsigned mount_calls, unmount_calls, forget_calls, removals, expected_removals;
static unsigned sync_calls, disconnects, connects, init_calls;
static bool expect_usb_owned_disconnect, disconnect_fails;
static pf_fatfs_volume_result_t mount_result = PF_FATFS_VOLUME_OK;

bool pf_sd_port_try_lock(void)
{
    ++try_lock_calls;
    if (lock_unavailable) return false;
    ++reservation_depth;
    return true;
}
void pf_sd_port_lock(void) { ++reservation_depth; }
void pf_sd_port_unlock(void)
{ TEST_ASSERT_NOT_EQUAL(0, reservation_depth); --reservation_depth; }
bool pf_sd_backend_is_initialized(void) { return media.initialized; }
uint32_t pf_sd_backend_block_count(void) { return 128u; }
void pf_sd_backend_get_info(pf_sd_info_t *info) { *info = media; }
pf_sd_link_mode_t pf_sd_backend_mode(void) { return PF_SD_LINK_NATIVE_SD; }
void pf_sd_backend_confirm_removed(void)
{
    ++removals;
    media.initialized = false;
}
pf_sd_result_t pf_sd_backend_sync(void)
{
    ++sync_calls;
    return media.initialized && sync_ok ? PF_SD_OK : PF_SD_NOT_READY;
}
pf_sd_result_t pf_sd_backend_read_blocks(uint32_t first, void *buffer, uint32_t count)
{
    (void)first;
    (void)buffer;
    (void)count;
    TEST_FAIL_MESSAGE("Automount must not read raw sectors through the descriptor fake");
    return PF_SD_IO_ERROR;
}
pf_sd_result_t pf_sd_backend_write_blocks(uint32_t first, const void *buffer, uint32_t count)
{
    (void)first;
    (void)buffer;
    (void)count;
    TEST_FAIL_MESSAGE("Automount must not write raw sectors through the descriptor fake");
    return PF_SD_IO_ERROR;
}
pf_sd_result_t pf_sd_backend_init(const pf_sd_config_t *config)
{
    TEST_ASSERT_EQUAL_PTR(&media_config, config);
    TEST_ASSERT_FALSE(usb_owned);
    TEST_ASSERT_NOT_EQUAL(0, reservation_depth);
    if (auto_mount_blocked) {
        TEST_ASSERT_NOT_EQUAL(0, disconnects);
        ++init_calls;
    }
    media.initialized = !init_fails;
    return media.initialized ? PF_SD_OK : PF_SD_NOT_READY;
}
bool pf_fatfs_volume_is_mounted(void) { return mounted; }
pf_fatfs_volume_result_t pf_fatfs_volume_unmount(FRESULT *result)
{
    (void)result;
    if (media.presence_known && !media.present) {
        TEST_ASSERT_EQUAL_UINT(expected_removals, removals);
        TEST_ASSERT_FALSE(media.initialized);
    }
    ++unmount_calls;
    mounted = false;
    return PF_FATFS_VOLUME_OK;
}
void pf_fatfs_volume_forget_filesystem_format(void) { ++forget_calls; }
pf_fatfs_volume_result_t pf_fatfs_volume_mount(const pf_block_device_t *device, FRESULT *result)
{
    TEST_ASSERT_EQUAL_PTR(pf_storage_firmware_device(), device);
    TEST_ASSERT_TRUE(media.initialized);
    if (result != NULL) {
        *result = mount_result == PF_FATFS_VOLUME_OK ? FR_OK : FR_NO_FILESYSTEM;
    }
    ++mount_calls;
    if (mount_result == PF_FATFS_VOLUME_OK) mounted = true;
    return mount_result;
}
uint32_t pf_system_uptime_ms(void) { return now; }
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout)
{
    const storage_request_t *request = item;
    TEST_ASSERT_EQUAL_PTR(requests, queue);
    TEST_ASSERT_TRUE(request->recover_media || request->unmount_for_usb);
    TEST_ASSERT_EQUAL_UINT32(0, timeout);
    if (queue_fails) return 0;
    queued_recovery = request->recover_media;
    queued_unmount = request->unmount_for_usb;
    return pdPASS;
}
bool tud_msc_set_sense(uint8_t lun, uint8_t key, uint8_t asc, uint8_t ascq)
{ (void)lun; (void)key; (void)asc; (void)ascq; return true; }
bool tud_disconnect(void)
{
    if (expect_usb_owned_disconnect) {
        TEST_ASSERT_TRUE(usb_owned);
        expect_usb_owned_disconnect = false;
    }
    ++disconnects;
    return !disconnect_fails;
}
bool tud_connect(void)
{
    ++connects;
    return true;
}
void sleep_ms(uint32_t ms) { (void)ms; }

static void poll(unsigned count)
{
    while (count-- != 0u) {
        now += MEDIA_SERVICE_POLL_MS;
        service_media();
        TEST_ASSERT_FALSE(media_service_busy);
        TEST_ASSERT_EQUAL_UINT(0, reservation_depth);
    }
}
