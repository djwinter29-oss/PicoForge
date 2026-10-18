#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/msc")
#define PF_FATFS_VOLUME_H
#define PF_STORAGE_WORKER_H
static bool pf_fatfs_volume_is_mounted(void);
static void pf_fatfs_volume_forget_filesystem_format(void);
static bool pf_storage_worker_busy(void);
static bool pf_storage_worker_media_absent(void);
static bool pf_storage_worker_recover_media(void);
static bool pf_storage_worker_return_from_usb(void);
static bool pf_storage_worker_unmount_for_usb(void);
#include "../../../src/app/usb/usb_msc.c"
#include "block_device.h"

static uint8_t media[4u * 512u];
static bool ready, usb_owner, mounted, worker_busy, sync_fails, write_fails, recovery_queued;
static unsigned depth, reads, writes, syncs, releases;
static uint8_t sense_key, sense_asc;
static bool firmware_context, host_context = true;

void pf_nor_media_lock(void) { ++depth; }
bool pf_nor_media_try_lock(void) { ++depth; return true; }
void pf_nor_media_unlock(void) { TEST_ASSERT_GREATER_THAN_UINT(0u, depth); --depth; }
bool pf_nor_media_ready(void) { return ready; }
uint32_t pf_nor_media_sector_count(void) { return ready ? 4u : 0u; }
pf_block_result_t pf_nor_media_acquire_usb(void)
{
    TEST_ASSERT_GREATER_THAN_UINT(0u, depth);
    TEST_ASSERT_FALSE(worker_busy);
    if (!ready || mounted) {
        return PF_BLOCK_NOT_READY;
    }
    usb_owner = true;
    return PF_BLOCK_OK;
}
pf_block_result_t pf_nor_media_release_usb(void)
{
    TEST_ASSERT_GREATER_THAN_UINT(0u, depth);
    usb_owner = false;
    ++releases;
    return PF_BLOCK_OK;
}
pf_block_result_t pf_nor_media_release_usb_for_recovery(void)
{
    TEST_ASSERT_FALSE(ready);
    return pf_nor_media_release_usb();
}

static pf_block_result_t geometry(void *context, pf_block_geometry_t *value)
{
    (void)context;
    if (!ready) return PF_BLOCK_NOT_READY;
    *value = (pf_block_geometry_t){.sector_count = 4u, .sector_size = 512u, .erase_sector_count = 1u};
    return PF_BLOCK_OK;
}
static pf_block_result_t read_blocks(void *context, uint32_t first, void *buffer, uint32_t count)
{
    TEST_ASSERT_TRUE(depth > 0u);
    if (!ready || usb_owner != *(bool *)context) return PF_BLOCK_NOT_READY;
    ++reads;
    memcpy(buffer, media + first * 512u, count * 512u);
    return PF_BLOCK_OK;
}
static pf_block_result_t write_blocks(void *context, uint32_t first, const void *buffer, uint32_t count)
{
    TEST_ASSERT_TRUE(depth > 0u);
    if (!ready || usb_owner != *(bool *)context) return PF_BLOCK_NOT_READY;
    ++writes;
    if (write_fails) return PF_BLOCK_IO_ERROR;
    memcpy(media + first * 512u, buffer, count * 512u);
    return PF_BLOCK_OK;
}
static pf_block_result_t sync_blocks(void *context)
{
    if (!ready || usb_owner != *(bool *)context) return PF_BLOCK_NOT_READY;
    ++syncs;
    return sync_fails ? PF_BLOCK_IO_ERROR : PF_BLOCK_OK;
}
static const pf_block_device_t *device_for_owner(bool usb)
{
    static const pf_block_device_ops_t ops = {
        .geometry = geometry, .read = read_blocks, .write = write_blocks, .sync = sync_blocks,
    };
    static const pf_block_device_t firmware = {.ops = &ops, .context = &firmware_context};
    static const pf_block_device_t host = {.ops = &ops, .context = &host_context};
    return usb ? &host : &firmware;
}
const pf_block_device_t *pf_nor_media_firmware_device(void) { return device_for_owner(false); }
const pf_block_device_t *pf_nor_media_usb_device(void) { return device_for_owner(true); }
static bool pf_fatfs_volume_is_mounted(void) { return mounted; }
static void pf_fatfs_volume_forget_filesystem_format(void) {}
static bool pf_storage_worker_busy(void) { return worker_busy; }
static bool pf_storage_worker_media_absent(void) { return false; }
static bool pf_storage_worker_unmount_for_usb(void)
{
    TEST_ASSERT_GREATER_THAN_UINT(0u, depth);
    TEST_ASSERT_FALSE(usb_owner || worker_busy);
    worker_busy = true;
    return true;
}
static bool pf_storage_worker_recover_media(void)
{
    TEST_ASSERT_GREATER_THAN_UINT(0u, depth);
    TEST_ASSERT_TRUE(usb_owner);
    recovery_queued = true;
    return true;
}
static bool pf_storage_worker_return_from_usb(void)
{
    TEST_ASSERT_TRUE(usb_owner);
    worker_busy = true;
    return true;
}
void __real_mscd_reset(uint8_t port) { (void)port; }
bool __real_mscd_control_xfer_cb(uint8_t port, uint8_t stage, tusb_control_request_t const *request)
{ (void)port; (void)stage; (void)request; return false; }
bool tud_msc_set_sense(uint8_t lun, uint8_t key, uint8_t asc, uint8_t ascq)
{ (void)lun; (void)ascq; sense_key = key; sense_asc = asc; return true; }

void setUp(void)
{
    memset(media, 0xff, sizeof(media));
    ready = true;
    usb_owner = mounted = worker_busy = sync_fails = write_fails = recovery_queued = false;
    msc_enabled = msc_ejected = removal_prevented = request_pending = transition_pending = false;
    transfer_sense_pending = recovery_requested = request_rejected = false;
    unmount_pending = force_return_requested = remount_pending = false;
    depth = reads = writes = syncs = releases = 0u;
}
void tearDown(void) { TEST_ASSERT_EQUAL_UINT(0u, depth); }

static void enable(void)
{
    pf_usb_msc_request(true);
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(msc_enabled && usb_owner && !request_rejected);
}

void test_force_return_nor_keeps_safe_disable_strict_and_awaits_mount(void)
{
    enable();
    removal_prevented = true;
    pf_usb_msc_request(false);
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(msc_enabled && request_rejected);
    pf_usb_msc_request_force_return();
    pf_usb_msc_task();
    TEST_ASSERT_FALSE(msc_enabled || usb_owner || msc_ejected || request_rejected);
    TEST_ASSERT_TRUE(transition_pending && worker_busy);
    TEST_ASSERT_EQUAL_UINT(1u, releases);
    mounted = true;
    worker_busy = false;
    pf_usb_msc_task();
    TEST_ASSERT_FALSE(transition_pending || request_rejected);
}

void test_force_return_nor_sync_failure_keeps_usb_ownership(void)
{
    enable();
    sync_fails = true;
    pf_usb_msc_request_force_return();
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(msc_enabled && usb_owner && request_rejected);
    TEST_ASSERT_FALSE(transition_pending || worker_busy);
    TEST_ASSERT_EQUAL_UINT(0u, releases);
}

void test_nor_handoff_unmounts_idle_media_and_requires_checked_eject(void)
{
    mounted = true;
    pf_usb_msc_request(true);
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(unmount_pending && transition_pending && worker_busy && !usb_owner);
    pf_usb_msc_task();
    TEST_ASSERT_FALSE(usb_owner);
    worker_busy = false;
    mounted = false;
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(msc_enabled && usb_owner && !request_rejected);
    TEST_ASSERT_TRUE(tud_msc_start_stop_cb(0u, 0u, false, true));
    pf_usb_msc_request(false);
    pf_usb_msc_task();
    TEST_ASSERT_FALSE(msc_enabled || usb_owner);
    releases = 0u;
    worker_busy = true;
    pf_usb_msc_request(true);
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(request_rejected && !usb_owner);
    worker_busy = false;
    enable();
    pf_usb_msc_request(false);
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(request_rejected && msc_enabled && usb_owner);
    TEST_ASSERT_TRUE(tud_msc_prevent_allow_medium_removal_cb(0u, 1u, 0u));
    TEST_ASSERT_FALSE(tud_msc_start_stop_cb(0u, 0u, false, true));
    TEST_ASSERT_TRUE(tud_msc_prevent_allow_medium_removal_cb(0u, 0u, 0u));
    sync_fails = true;
    TEST_ASSERT_FALSE(tud_msc_start_stop_cb(0u, 0u, false, true));
    TEST_ASSERT_FALSE(msc_ejected);
    sync_fails = false;
    TEST_ASSERT_TRUE(tud_msc_start_stop_cb(0u, 0u, false, true));
    sync_fails = true;
    pf_usb_msc_request(false);
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(request_rejected && usb_owner && msc_enabled);
    sync_fails = false;
    pf_usb_msc_request(false);
    pf_usb_msc_task();
    TEST_ASSERT_FALSE(msc_enabled || usb_owner);
    TEST_ASSERT_EQUAL_UINT(1u, releases);
}

void test_failed_nor_unmount_retains_firmware_ownership_and_mounted_volume(void)
{
    mounted = true;
    pf_usb_msc_request(true);
    pf_usb_msc_task();
    worker_busy = false;
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(request_rejected && mounted && !usb_owner && !msc_enabled);
    TEST_ASSERT_FALSE(transition_pending || request_pending);
}

void test_disable_during_nor_unmount_waits_for_completion_without_exposing_media(void)
{
    mounted = true;
    pf_usb_msc_request(true);
    pf_usb_msc_task();
    pf_usb_msc_request(false);
    pf_usb_msc_task();
    TEST_ASSERT_FALSE(msc_enabled || usb_owner);
    mounted = false;
    worker_busy = false;
    pf_usb_msc_task();
    TEST_ASSERT_FALSE(msc_enabled || usb_owner || request_rejected || request_pending);
}

void test_nor_msc_preserves_partial_and_batch_bytes_bounds_and_failed_write_no_replay(void)
{
    uint8_t bytes[1024], output[1024];
    memset(bytes, 0x5a, sizeof(bytes));
    enable();
    TEST_ASSERT_EQUAL_INT(1024, tud_msc_write10_cb(0u, 1u, 0u, bytes, sizeof(bytes)));
    TEST_ASSERT_EQUAL_INT(1024, tud_msc_read10_cb(0u, 1u, 0u, output, sizeof(output)));
    TEST_ASSERT_EQUAL_MEMORY(bytes, output, sizeof(bytes));
    memset(bytes, 0x33, 7u);
    TEST_ASSERT_EQUAL_INT(7, tud_msc_write10_cb(0u, 1u, 3u, bytes, 7u));
    TEST_ASSERT_EQUAL_HEX8(0x5a, media[512u + 2u]);
    TEST_ASSERT_EQUAL_MEMORY(bytes, media + 515u, 7u);
    TEST_ASSERT_EQUAL_HEX8(0x5a, media[512u + 10u]);
    unsigned before = writes + reads;
    TEST_ASSERT_EQUAL_INT(-1, tud_msc_write10_cb(0u, 3u, 0u, output, sizeof(output)));
    TEST_ASSERT_EQUAL_UINT(before, writes + reads);
    unsigned before_syncs = syncs, before_writes = writes;
    write_fails = true;
    TEST_ASSERT_EQUAL_INT(-1, tud_msc_write10_cb(0u, 0u, 0u, output, sizeof(output)));
    TEST_ASSERT_EQUAL_UINT(before_writes + 1u, writes);
    TEST_ASSERT_EQUAL_UINT(before_syncs, syncs);
    TEST_ASSERT_EQUAL_UINT8(SCSI_SENSE_MEDIUM_ERROR, sense_key);
    TEST_ASSERT_EQUAL_UINT8(0x0c, sense_asc);
    ready = false;
    TEST_ASSERT_FALSE(tud_msc_test_unit_ready_cb(0u));
    pf_usb_msc_request_recovery();
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(recovery_queued && !usb_owner && !msc_enabled);
}

void test_recovery_rejects_healthy_busy_mounted_or_prevented_media_without_release_or_io(void)
{
    enable();
    for (unsigned scenario = 0u; scenario < 4u; ++scenario) {
        ready = scenario == 0u;
        worker_busy = scenario == 1u;
        mounted = scenario == 2u;
        removal_prevented = scenario == 3u;
        pf_usb_msc_request_recovery();
        pf_usb_msc_task();
        TEST_ASSERT_TRUE(request_rejected && usb_owner && msc_enabled);
        TEST_ASSERT_FALSE(recovery_queued);
        TEST_ASSERT_EQUAL_UINT(0u, reads + writes + syncs + releases);
    }
}
