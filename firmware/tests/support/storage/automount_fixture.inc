#include "unity.h"
#include <string.h>
#include "drivers/sd_card/sd_msc.h"
#include "drivers/sd_card/sd_port.h"
#include "drivers/sd_card/sd_native.h"
#define WORKER_SOURCE "../../../src/app/storage/storage_worker.c"
#define FACADE_SOURCE "../../../src/drivers/sd_card/sd.c"
#define MSC_SOURCE "../../../src/usb/msc.c"
#include WORKER_SOURCE
#include FACADE_SOURCE
#include MSC_SOURCE

static unsigned reservation_depth, try_lock_calls;
static bool lock_unavailable;
static pf_sd_info_t media = { .presence_known = true, .present = true };
static uint32_t now;
static bool mounted, queue_fails, queued_recovery, sync_ok = true, init_fails;
static unsigned mount_calls, unmount_calls, forget_calls, removals, expected_removals;
static unsigned sync_calls, disconnects, init_calls;
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
bool pf_sd_native_is_initialized(void) { return media.initialized; }
uint32_t pf_sd_native_block_count(void) { return 128u; }
void pf_sd_native_get_info(pf_sd_info_t *info) { *info = media; }
pf_sd_native_init_stage_t pf_sd_native_get_init_stage(void)
{ return PF_SD_NATIVE_INIT_STAGE_NONE; }
void pf_sd_get_profile(pf_sd_profile_t *profile)
{ *profile = (pf_sd_profile_t){0}; }
void pf_sd_native_confirm_card_removed(void)
{
    ++removals;
    media.initialized = false;
}
pf_sd_result_t pf_sd_native_sync(void)
{
    ++sync_calls;
    return media.initialized && sync_ok ? PF_SD_OK : PF_SD_NOT_READY;
}
pf_sd_result_t pf_sd_native_init(const pf_sd_config_t *config)
{
    TEST_ASSERT_EQUAL_PTR(&card_config, config);
    TEST_ASSERT_EQUAL(PF_SD_OWNER_FIRMWARE, active_owner);
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
static const pf_block_device_t block_device = {0};
const pf_block_device_t *pf_sd_block_device(void) { return &block_device; }
pf_fatfs_volume_result_t pf_fatfs_volume_mount(const pf_block_device_t *device, FRESULT *result)
{
    TEST_ASSERT_EQUAL_PTR(&block_device, device);
    TEST_ASSERT_TRUE(media.initialized);
    (void)result;
    ++mount_calls;
    if (mount_result == PF_FATFS_VOLUME_OK) mounted = true;
    return mount_result;
}
uint32_t pf_system_uptime_ms(void) { return now; }
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout)
{
    const storage_request_t *request = item;
    TEST_ASSERT_EQUAL_PTR(requests, queue);
    TEST_ASSERT_TRUE(request->recover_card);
    TEST_ASSERT_EQUAL_UINT32(0, timeout);
    if (queue_fails) return 0;
    queued_recovery = true;
    return pdPASS;
}
bool tud_msc_set_sense(uint8_t lun, uint8_t key, uint8_t asc, uint8_t ascq)
{ (void)lun; (void)key; (void)asc; (void)ascq; return true; }
bool tud_disconnect(void) { ++disconnects; return true; }
bool tud_connect(void) { return true; }
void sleep_ms(uint32_t ms) { (void)ms; }

static void poll(unsigned count)
{
    while (count-- != 0u) {
        now += CARD_PRESENCE_POLL_MS;
        service_card_presence();
        TEST_ASSERT_FALSE(card_service_busy);
        TEST_ASSERT_EQUAL_UINT(0, reservation_depth);
    }
}
