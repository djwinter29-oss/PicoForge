#include "unity.h"
#include "Mocklogging.h"
#include "block_device.h"
TEST_INCLUDE_PATH("firmware/tests/support/storage")
#include "storage/automount_fixture.inc"

void setUp(void)
{
    pf_log_write_Ignore();
    config_available = configured = transport_selected = true;
    requests = (void *)1;
    media_config.transport.mode = PF_STORAGE_BUS_NATIVE_SD;
}
void tearDown(void) { TEST_ASSERT_EQUAL_UINT(0, reservation_depth); }

void test_debounce_retry_wrap_ownership_removal_and_recovery_lifecycle(void)
{
    poll(2);
    TEST_ASSERT_EQUAL_UINT(0, mount_calls);
    init_fails = true;
    poll(1);
    TEST_ASSERT_EQUAL_UINT(0, mount_calls);
    TEST_ASSERT_TRUE(auto_mount_pending && mount_retry_waiting && !mounted);
    init_fails = false;
    poll(50);
    TEST_ASSERT_TRUE(mount_calls == 1 && mounted);
    poll(60);
    TEST_ASSERT_EQUAL_UINT(1, mount_calls);
    mounted = false;
    auto_mount_pending = false;
    poll(60);
    TEST_ASSERT_EQUAL_UINT(1, mount_calls);

    media.present = false;
    poll(2);
    TEST_ASSERT_EQUAL_UINT(0, removals);
    media.present = true;
    poll(1);
    TEST_ASSERT_EQUAL_UINT(0, removals);
    media.present = false;
    poll(2);
    TEST_ASSERT_EQUAL_UINT(0, removals);
    poll(1);
    TEST_ASSERT_TRUE(removals == 1 && forget_calls == 1);
    media.present = true;
    mount_result = PF_FATFS_VOLUME_FILESYSTEM_ERROR;
    poll(3);
    TEST_ASSERT_TRUE(mount_calls == 2 && auto_mount_pending);
    poll(49);
    TEST_ASSERT_EQUAL_UINT(2, mount_calls);
    mount_result = PF_FATFS_VOLUME_OK;
    poll(1);
    TEST_ASSERT_TRUE(mount_calls == 3 && mounted);

    media.present = false;
    expected_removals = removals + 1u;
    TEST_ASSERT_TRUE(media.initialized);
    poll(3);
    TEST_ASSERT_TRUE(unmount_calls == 1 && !mounted);
    media.present = true;
    poll(1);
    media.present = false;
    poll(1);
    media.present = true;
    poll(2);
    TEST_ASSERT_EQUAL_UINT(3, mount_calls);
    poll(1);
    TEST_ASSERT_TRUE(mount_calls == 4 && mounted);

    mounted = false;
    reinit_after_usb_owner = true;
    active_owner = PF_SD_MEDIA_OWNER_USB_MSC;
    poll(60);
    TEST_ASSERT_TRUE(mount_calls == 4 && reinit_after_usb_owner);
    media.present = false;
    poll(2);
    TEST_ASSERT_EQUAL_UINT(2, removals);
    poll(1);
    TEST_ASSERT_TRUE(removals == 3 && !media.initialized);
    media.present = true;
    poll(3);
    TEST_ASSERT_TRUE(mount_calls == 4 && !media.initialized);
    active_owner = PF_SD_MEDIA_OWNER_FIRMWARE;
    busy = true;
    poll(3);
    TEST_ASSERT_TRUE(mount_calls == 4 && reinit_after_usb_owner);
    busy = false;
    poll(1);
    TEST_ASSERT_TRUE(mount_calls == 5 && mounted && !reinit_after_usb_owner);

    mounted = false;
    auto_mount_pending = mount_retry_waiting = true;
    last_mount_attempt_ms = UINT32_MAX - 1000u;
    now = 999u;
    service_media();
    TEST_ASSERT_EQUAL_UINT(5, mount_calls);
    now = 4000u;
    service_media();
    TEST_ASSERT_TRUE(mount_calls == 6 && mounted);

    mounted = false;
    auto_mount_pending = true;
    mount_retry_waiting = stable_presence_valid = presence_sample_valid = false;
    mount_result = PF_FATFS_VOLUME_FILESYSTEM_ERROR;
    media = (pf_sd_info_t){0};
    unsigned previous_mounts = mount_calls;
    now = 0u;
    service_media();
    TEST_ASSERT_TRUE(mount_calls == previous_mounts + 1u && mount_retry_waiting);
    now = MEDIA_MOUNT_RETRY_MS - 1u;
    service_media();
    TEST_ASSERT_EQUAL_UINT(previous_mounts + 1u, mount_calls);
    now = MEDIA_MOUNT_RETRY_MS;
    mount_result = PF_FATFS_VOLUME_OK;
    service_media();
    TEST_ASSERT_TRUE(mount_calls == previous_mounts + 2u && mounted);

    TEST_ASSERT_TRUE(mounted && media.initialized && !auto_mount_pending);
    pf_usb_msc_request(true);
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(queued_unmount && busy && mounted && transition_pending);
    TEST_ASSERT_FALSE(msc_enabled);
    unmount_for_usb();
    TEST_ASSERT_FALSE(busy || mounted || auto_mount_pending);
    poll(3);
    TEST_ASSERT_FALSE(mounted);
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(msc_enabled && active_owner == PF_SD_MEDIA_OWNER_USB_MSC);
    poll(3);
    TEST_ASSERT_TRUE(!mounted && !auto_mount_pending);
    TEST_ASSERT_TRUE(tud_msc_start_stop_cb(0, 0, false, true));
    pf_usb_msc_request(false);
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(!msc_enabled && active_owner == PF_SD_MEDIA_OWNER_FIRMWARE &&
                     reinit_after_usb_owner);
    busy = true;
    poll(1);
    TEST_ASSERT_TRUE(reinit_after_usb_owner && !mounted);
    busy = false;
    previous_mounts = mount_calls;
    poll(1);
    TEST_ASSERT_TRUE(!reinit_after_usb_owner && mounted && media.initialized && !auto_mount_pending);
    TEST_ASSERT_EQUAL_UINT(previous_mounts + 1u, mount_calls);
    poll(60);
    TEST_ASSERT_TRUE(mounted && mount_calls == previous_mounts + 1u);

    media = (pf_sd_info_t){ .presence_known = true, .present = true };
    mounted = true;
    stable_presence_valid = presence_sample_valid = true;
    stable_card_present = presence_sample = true;
    presence_sample_count = CARD_PRESENCE_DEBOUNCE_SAMPLES;
    auto_mount_pending = mount_retry_waiting = false;
    previous_mounts = mount_calls;
    unsigned previous_unmounts = unmount_calls;
    service_media();
    TEST_ASSERT_EQUAL_UINT(previous_unmounts + 1u, unmount_calls);
    TEST_ASSERT_TRUE(mount_calls == previous_mounts + 1u && mounted && media.initialized);

    mounted = false;
    pf_usb_msc_request(true);
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(msc_enabled && active_owner == PF_SD_MEDIA_OWNER_USB_MSC && !msc_ejected);
    pf_usb_msc_request(false);
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(request_rejected && msc_enabled);
    sync_ok = false;
    TEST_ASSERT_FALSE(tud_msc_start_stop_cb(0, 0, false, true));
    sync_ok = true;
    TEST_ASSERT_TRUE(tud_msc_prevent_allow_medium_removal_cb(0, 1, 0));
    TEST_ASSERT_FALSE(tud_msc_start_stop_cb(0, 0, false, true));
    pf_usb_msc_request(false);
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(msc_enabled && request_rejected);
    TEST_ASSERT_TRUE(tud_msc_prevent_allow_medium_removal_cb(0, 0, 0));
    TEST_ASSERT_TRUE(tud_msc_start_stop_cb(0, 0, true, true));
    unsigned old_removals = removals;
    media.present = false;
    poll(2);
    TEST_ASSERT_TRUE(!pf_storage_worker_media_absent() && removals == old_removals);
    pf_usb_msc_request(false);
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(msc_enabled && request_rejected);
    poll(1);
    TEST_ASSERT_TRUE(pf_storage_worker_media_absent() && removals == old_removals + 1u);
    unsigned old_syncs = sync_calls;
    TEST_ASSERT_TRUE(tud_msc_prevent_allow_medium_removal_cb(0, 1, 0));
    pf_usb_msc_request(false);
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(msc_enabled && request_rejected);
    TEST_ASSERT_TRUE(tud_msc_prevent_allow_medium_removal_cb(0, 0, 0));
    pf_usb_msc_request(false);
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(!msc_enabled && active_owner == PF_SD_MEDIA_OWNER_FIRMWARE &&
                     reinit_after_usb_owner);
    TEST_ASSERT_EQUAL_UINT(old_syncs, sync_calls);
    previous_mounts = mount_calls;
    media.present = true;
    poll(2);
    TEST_ASSERT_EQUAL_UINT(previous_mounts, mount_calls);
    poll(1);
    TEST_ASSERT_TRUE(mount_calls == previous_mounts + 1u && media.initialized &&
                     !reinit_after_usb_owner);

    mounted = false;
    pf_usb_msc_request(true);
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(msc_enabled && active_owner == PF_SD_MEDIA_OWNER_USB_MSC);
    unsigned before_disconnects = disconnects;
    pf_usb_msc_request_recovery();
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(request_rejected && !queued_recovery && disconnects == before_disconnects);
    media.initialized = false;
    poll(100);
    TEST_ASSERT_FALSE(media.initialized);
    TEST_ASSERT_FALSE(tud_msc_start_stop_cb(0, 0, false, true));
    pf_usb_msc_request(false);
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(msc_enabled && request_rejected);
    unsigned before_syncs = sync_calls;
    lock_unavailable = true;
    unsigned before_reservations = try_lock_calls;
    pf_usb_msc_task();
    TEST_ASSERT_EQUAL_UINT(before_reservations, try_lock_calls);
    pf_usb_msc_request_recovery();
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(request_pending && transition_pending && !queued_recovery);
    lock_unavailable = false;
    busy = true;
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(request_rejected && !queued_recovery);
    busy = false;
    removal_prevented = true;
    pf_usb_msc_request_recovery();
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(request_rejected && !queued_recovery);
    removal_prevented = false;
    queue_fails = true;
    pf_usb_msc_request_recovery();
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(request_rejected && !queued_recovery && !busy && msc_enabled);
    queue_fails = false;
    pf_usb_msc_request_recovery();
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(!msc_enabled && !request_rejected && queued_recovery && busy);
    TEST_ASSERT_TRUE(active_owner == PF_SD_MEDIA_OWNER_FIRMWARE &&
                     disconnects == before_disconnects + 1u);
    TEST_ASSERT_EQUAL_UINT(before_syncs, sync_calls);
    queued_recovery = false;
    recover_media();
    TEST_ASSERT_TRUE(init_calls == 1 && !busy && media.initialized && !mounted && auto_mount_blocked);
    previous_mounts = mount_calls;
    poll(100);
    TEST_ASSERT_TRUE(!mounted && mount_calls == previous_mounts);
    pf_usb_msc_request(true);
    pf_usb_msc_task();
    TEST_ASSERT_TRUE(msc_enabled);
    TEST_ASSERT_TRUE(tud_msc_start_stop_cb(0, 0, false, true));
    pf_usb_msc_request(false);
    pf_usb_msc_task();
    poll(100);
    TEST_ASSERT_TRUE(!msc_enabled && !mounted && mount_calls == previous_mounts);
    media.present = false;
    poll(3);
    media.present = true;
    poll(100);
    TEST_ASSERT_TRUE(!mounted && mount_calls == previous_mounts && init_calls == 1);
    init_fails = true;
    busy = true;
    recover_media();
    poll(100);
    TEST_ASSERT_TRUE(!busy && !media.initialized && !mounted && init_calls == 2);
    TEST_ASSERT_EQUAL_UINT(previous_mounts, mount_calls);
}

void test_command_cancellation_keeps_reservation_until_completion(void)
{
 busy=true; active_session=42; cancelled=false; benchmark_active=false;
 pf_storage_worker_cancel(41);
 TEST_ASSERT_FALSE(cancelled);
 pf_storage_worker_cancel(42);
 TEST_ASSERT_TRUE(cancelled);
 TEST_ASSERT_TRUE(pf_storage_worker_busy());
 cancelled=false; benchmark_active=true;
 pf_storage_worker_cancel(42);
 TEST_ASSERT_FALSE(cancelled);
 benchmark_finished=true;
 TEST_ASSERT_FALSE(pf_storage_worker_busy());
 TEST_ASSERT_FALSE(benchmark_active);
 TEST_ASSERT_FALSE(benchmark_finished);
 pf_storage_worker_cancel(42);
 TEST_ASSERT_FALSE(cancelled);
}

void test_forced_sd_return_detaches_before_release_and_attempts_mount_once(void)
{
 busy=media_service_busy=benchmark_active=benchmark_finished=false;
 mounted=false;
 media=(pf_sd_info_t){.present=true,.presence_known=true,.initialized=true};
 active_owner=PF_SD_MEDIA_OWNER_USB_MSC;
 msc_enabled=true;
 msc_ejected=transition_pending=request_pending=unmount_pending=remount_pending=false;
 recovery_requested=force_return_requested=false;
 removal_prevented=true;
 sync_ok=true;
 init_fails=queue_fails=false;
 mount_result=PF_FATFS_VOLUME_OK;
 disconnect_fails=true;
 expect_usb_owned_disconnect=true;
 pf_usb_msc_request_force_return();
 pf_usb_msc_task();
 TEST_ASSERT_TRUE(msc_enabled && request_rejected);
 TEST_ASSERT_FALSE(busy || remount_pending);
 TEST_ASSERT_EQUAL(PF_SD_MEDIA_OWNER_USB_MSC,active_owner);
 disconnect_fails=false;
 queue_fails=true;
 pf_usb_msc_request_force_return();
 pf_usb_msc_task();
 TEST_ASSERT_TRUE(msc_enabled && request_rejected);
 TEST_ASSERT_FALSE(busy || remount_pending);
 TEST_ASSERT_EQUAL(PF_SD_MEDIA_OWNER_USB_MSC,active_owner);
 queue_fails=false;
 unsigned before_disconnects=disconnects;
 expect_usb_owned_disconnect=true;
 pf_usb_msc_request_force_return();
 pf_usb_msc_task();
 TEST_ASSERT_TRUE(busy && remount_pending && transition_pending && !msc_enabled);
 TEST_ASSERT_EQUAL_UINT(before_disconnects+1u,disconnects);
 TEST_ASSERT_EQUAL(PF_SD_MEDIA_OWNER_FIRMWARE,active_owner);
 return_from_usb();
 pf_usb_msc_task();
 TEST_ASSERT_TRUE(mounted && !request_rejected && !transition_pending);
 TEST_ASSERT_FALSE(auto_mount_blocked || reinit_after_usb_owner);

 mounted=false;
 active_owner=PF_SD_MEDIA_OWNER_USB_MSC;
 msc_enabled=true;
 mount_result=PF_FATFS_VOLUME_FILESYSTEM_ERROR;
 pf_usb_msc_request_force_return();
 pf_usb_msc_task();
 return_from_usb();
 pf_usb_msc_task();
 TEST_ASSERT_TRUE(request_rejected && auto_mount_blocked);
 TEST_ASSERT_FALSE(msc_enabled || mounted || transition_pending || auto_mount_pending);
 unsigned before_mounts=mount_calls;
 poll(100);
 TEST_ASSERT_EQUAL_UINT(before_mounts,mount_calls);
 active_owner=PF_SD_MEDIA_OWNER_USB_MSC;
 msc_enabled=true;
 init_fails=true;
 pf_usb_msc_request_force_return();
 pf_usb_msc_task();
 return_from_usb();
 pf_usb_msc_task();
 TEST_ASSERT_TRUE(request_rejected && auto_mount_blocked);
 TEST_ASSERT_FALSE(media.initialized || mounted || msc_enabled || transition_pending);
 TEST_ASSERT_EQUAL_UINT(before_mounts,mount_calls);
 poll(100);
 TEST_ASSERT_EQUAL_UINT(before_mounts,mount_calls);
}
