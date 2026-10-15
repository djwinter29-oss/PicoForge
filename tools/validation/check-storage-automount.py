#!/usr/bin/env python3
"""Exercise the actual storage worker card-presence service without hardware."""
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORKER = ROOT / "firmware/src/middleware/storage/storage_worker.c"


def function_source(source, name):
    match = re.search(r"^(?:static )?[\w]+ " + name + r"\(", source, re.MULTILINE)
    if match is None:
        raise ValueError(name)
    start = match.start()
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        depth += {"{": 1, "}": -1}.get(source[index], 0)
        if depth == 0:
            return source[start:index + 1]
    raise ValueError(name)


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
static unsigned reservation_depth;
static void pf_sd_card_lock(void) { ++reservation_depth; }
static void pf_sd_card_unlock(void) { assert(reservation_depth); --reservation_depth; }
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
typedef enum { PF_SD_CARD_OWNER_FIRMWARE, PF_SD_CARD_OWNER_USB_MSC } owner_t;
typedef enum {
    PF_STORAGE_OK, PF_STORAGE_ALREADY_MOUNTED, PF_STORAGE_FILESYSTEM_ERROR
} pf_storage_result_t;
typedef struct { bool presence_known, present, initialized; } pf_sd_card_info_t;
static int card_config;
static bool config_available = true, busy, card_service_busy;
@STATE@
static owner_t active_owner = PF_SD_CARD_OWNER_FIRMWARE;
static pf_sd_card_info_t media = { true, true, false };
static uint32_t now;
static bool mounted, reinit_request;
static unsigned mount_calls, unmount_calls, forget_calls, removals, expected_removals;
static pf_storage_result_t mount_result = PF_STORAGE_OK;
typedef owner_t pf_sd_card_owner_t;
static bool pf_sd_card_is_initialized(void) { return media.initialized; }
static void pf_sd_card_get_info(pf_sd_card_info_t *info) { *info = media; }
static bool pf_storage_is_mounted(void) { return mounted; }
static pf_storage_result_t pf_storage_unmount(void *result)
{
    (void)result;
    if (media.presence_known && !media.present) {
        assert(removals == expected_removals && !media.initialized);
    }
    ++unmount_calls;
    mounted = false;
    return PF_STORAGE_OK;
}
static void pf_storage_forget_filesystem_format(void) { ++forget_calls; }
typedef enum { PF_SD_CARD_OK, PF_SD_CARD_INVALID_ARGUMENT, PF_SD_CARD_NOT_READY, PF_SD_CARD_NOT_CONFIGURED } pf_sd_card_result_t;
#define PF_SD_CARD_OPERATION_MSC_ACQUIRE 5u
static pf_sd_card_result_t record_failure(unsigned operation, pf_sd_card_result_t result)
{ (void)operation; return result; }
#define PF_CONFIG_SD_CARD_TRANSPORT 0
#define PF_CONFIG_SD_CARD_TRANSPORT_SPI 1
#define card_initialized media.initialized
static bool transport_selected = true;
typedef struct { void *pio; bool write_stream_open, read_stream_unresolved, mode_restore_failed; } pf_sdio_pio_host_t;
static pf_sdio_pio_host_t sdio_pio_host = { .pio = (void *)1 };
static bool restore_command_mode(pf_sdio_pio_host_t *host)
{
    assert(!host->write_stream_open && !host->read_stream_unresolved);
    ++removals;
    host->mode_restore_failed = false;
    return true;
}
static void pf_sdio_pio_host_deinit(pf_sdio_pio_host_t *host) { host->pio = NULL; }
@FACADE@
#define SCSI_SENSE_NOT_READY 2u
#define SCSI_SENSE_ILLEGAL_REQUEST 5
#define SCSI_SENSE_MEDIUM_ERROR 3
static void tud_msc_set_sense(unsigned l, unsigned s, unsigned a, unsigned q)
{ (void)l; (void)s; (void)a; (void)q; }
static bool media_ready(unsigned lun) { return lun == 0 && media.initialized; }
static bool msc_enabled, msc_ejected, removal_prevented, request_pending;
static bool requested_enabled, transition_pending, request_rejected;
static bool sync_ok = true;
static unsigned sync_calls;
static bool pf_storage_worker_busy(void) { return busy || card_service_busy; }
static bool pf_storage_worker_media_absent(void);
static bool tud_disconnect(void) { return true; }
static bool tud_connect(void) { return true; }
static void sleep_ms(unsigned ms) { (void)ms; }
static pf_sd_card_result_t pf_sd_card_msc_sync(void)
{
    ++sync_calls;
    return media.initialized && sync_ok ? PF_SD_CARD_OK : PF_SD_CARD_NOT_READY;
}
static uint32_t pf_system_uptime_ms(void) { return now; }
static pf_storage_result_t pf_storage_mount(const int *config, void *result)
{
    (void)config;
    (void)result;
    ++mount_calls;
    if (mount_result == PF_STORAGE_OK) {
        mounted = media.initialized = true;
    }
    return mount_result;
}
@ATTEMPT@
@FUNCTION@
@MSC@
static void poll(unsigned count)
{
    while (count-- != 0u) {
        now += CARD_PRESENCE_POLL_MS;
        service_card_presence();
        assert(!card_service_busy);
    }
}
int main(void)
{
    poll(2);
    assert(mount_calls == 0);
    poll(1);
    assert(mount_calls == 1 && mounted);
    poll(60);
    assert(mount_calls == 1);

    mounted = false;
    auto_mount_pending = false;
    poll(60);
    assert(mount_calls == 1);

    media.present = false;
    poll(2);
    assert(removals == 0u); /* suspicious samples cannot abandon protocol state */
    media.present = true;
    poll(1);
    assert(removals == 0u);
    media.present = false;
    poll(2);
    assert(removals == 0u);
    poll(1);
    assert(removals == 1u && forget_calls == 1);
    media.present = true;
    mount_result = PF_STORAGE_FILESYSTEM_ERROR;
    poll(3);
    assert(mount_calls == 2 && auto_mount_pending);
    poll(49);
    assert(mount_calls == 2);
    mount_result = PF_STORAGE_OK;
    poll(1);
    assert(mount_calls == 3 && mounted);

    media.present = false;
    expected_removals = removals + 1u;
    assert(media.initialized);
    poll(3);
    assert(unmount_calls == 1 && !mounted);
    media.present = true;
    poll(1);
    media.present = false;
    poll(1);
    media.present = true;
    poll(2);
    assert(mount_calls == 3);
    poll(1);
    assert(mount_calls == 4 && mounted);

    mounted = false;
    reinit_request = true;
    active_owner = PF_SD_CARD_OWNER_USB_MSC;
    poll(60);
    assert(mount_calls == 4 && reinit_request);
    media.present = false;
    poll(2);
    assert(removals == 2u);
    poll(1);
    assert(removals == 3u && !media.initialized);
    media.present = true;
    poll(3);
    assert(mount_calls == 4u && !media.initialized);
    active_owner = PF_SD_CARD_OWNER_FIRMWARE;
    busy = true;
    poll(3);
    assert(mount_calls == 4 && reinit_request);
    busy = false;
    poll(1);
    assert(mount_calls == 5 && mounted && !reinit_request);

    mounted = false;
    auto_mount_pending = true;
    mount_retry_waiting = true;
    last_mount_attempt_ms = UINT32_MAX - 1000u;
    now = 999u;
    service_card_presence();
    assert(mount_calls == 5);
    now = 4000u;
    service_card_presence();
    assert(mount_calls == 6 && mounted);

    mounted = false;
    auto_mount_pending = true;
    mount_retry_waiting = false;
    stable_presence_valid = presence_sample_valid = false;
    mount_result = PF_STORAGE_FILESYSTEM_ERROR;
    media = (pf_sd_card_info_t){ false, false, false };
    unsigned previous_mounts = mount_calls;
    now = 0u;
    service_card_presence();
    assert(mount_calls == previous_mounts + 1u && mount_retry_waiting);
    now = CARD_MOUNT_RETRY_MS - 1u;
    service_card_presence();
    assert(mount_calls == previous_mounts + 1u);
    now = CARD_MOUNT_RETRY_MS;
    mount_result = PF_STORAGE_OK;
    service_card_presence();
    assert(mount_calls == previous_mounts + 2u && mounted);

    /* No detect pin: initial mount completed; real MSC ownership return. */
    assert(mounted && media.initialized && !auto_mount_pending);
    mounted = false; /* firmware unmounts before handing media to USB */
    pf_usb_msc_request(true);
    pf_usb_msc_task();
    assert(msc_enabled && active_owner == PF_SD_CARD_OWNER_USB_MSC);
    poll(3);
    assert(!mounted && !auto_mount_pending);
    assert(tud_msc_start_stop_cb(0u, 0u, false, true));
    pf_usb_msc_request(false);
    pf_usb_msc_task();
    assert(!msc_enabled && active_owner == PF_SD_CARD_OWNER_FIRMWARE && reinit_request);
    busy = true;
    poll(1);
    assert(reinit_request && !mounted);
    busy = false;
    previous_mounts = mount_calls;
    poll(1);
    assert(!reinit_request && mounted && media.initialized && !auto_mount_pending);
    assert(mount_calls == previous_mounts + 1u);
    poll(60);
    assert(mounted && mount_calls == previous_mounts + 1u);

    media = (pf_sd_card_info_t){ true, true, false };
    mounted = true;
    stable_presence_valid = presence_sample_valid = true;
    stable_card_present = presence_sample = true;
    presence_sample_count = CARD_PRESENCE_DEBOUNCE_SAMPLES;
    auto_mount_pending = false;
    mount_retry_waiting = false;
    previous_mounts = mount_calls;
    unsigned previous_unmounts = unmount_calls;
    service_card_presence();
    assert(unmount_calls == previous_unmounts + 1u);
    assert(mount_calls == previous_mounts + 1u && mounted && media.initialized);
    /* Connected production worker -> facade -> host abandonment -> MSC release. */
    mounted = false;
    pf_usb_msc_request(true);
    pf_usb_msc_task();
    assert(msc_enabled && active_owner == PF_SD_CARD_OWNER_USB_MSC && !msc_ejected);
    pf_usb_msc_request(false);
    pf_usb_msc_task();
    assert(request_rejected && msc_enabled); /* present media needs eject */
    sync_ok = false;
    assert(!tud_msc_start_stop_cb(0u, 0u, false, true));
    sync_ok = true;
    assert(tud_msc_prevent_allow_medium_removal_cb(0u, 1u, 0u));
    assert(!tud_msc_start_stop_cb(0u, 0u, false, true));
    pf_usb_msc_request(false);
    pf_usb_msc_task();
    assert(msc_enabled && request_rejected); /* prevention still applies */
    assert(tud_msc_prevent_allow_medium_removal_cb(0u, 0u, 0u));
    assert(tud_msc_start_stop_cb(0u, 0u, true, true));
    sdio_pio_host.write_stream_open = sdio_pio_host.read_stream_unresolved = true;
    unsigned old_removals = removals;
    media.present = false;
    poll(2);
    assert(!pf_storage_worker_media_absent() && removals == old_removals);
    pf_usb_msc_request(false);
    pf_usb_msc_task();
    assert(msc_enabled && request_rejected);
    poll(1);
    assert(pf_storage_worker_media_absent() && removals == old_removals + 1u);
    assert(!sdio_pio_host.write_stream_open && !sdio_pio_host.read_stream_unresolved);
    unsigned old_syncs = sync_calls;
    assert(tud_msc_prevent_allow_medium_removal_cb(0u, 1u, 0u));
    pf_usb_msc_request(false);
    pf_usb_msc_task();
    assert(msc_enabled && request_rejected);
    assert(tud_msc_prevent_allow_medium_removal_cb(0u, 0u, 0u));
    pf_usb_msc_request(false);
    pf_usb_msc_task();
    assert(!msc_enabled && active_owner == PF_SD_CARD_OWNER_FIRMWARE && reinit_request);
    assert(sync_calls == old_syncs); /* absent media cannot be synced */
    previous_mounts = mount_calls;
    media.present = true;
    poll(2);
    assert(mount_calls == previous_mounts);
    poll(1);
    assert(mount_calls == previous_mounts + 1u && media.initialized && !reinit_request);
    puts("Storage/MSC/facade/host removal lifecycle checks passed");
}
"""


def main():
    source = WORKER.read_text()
    state = source[source.index("static bool presence_sample_valid;"):
                   source.index("static void refresh_self_test_completion")]
    defines = "\n".join(re.findall(r"^#define CARD_.*$", source, re.MULTILINE))
    facade = (ROOT / "firmware/src/drivers/sd_card/sd_card.c").read_text()
    card = (ROOT / "firmware/src/drivers/sd_card/sdio_pio/sdio_pio.c").read_text()
    host = (ROOT / "firmware/src/drivers/sd_card/sdio_pio/sdio_pio_host.c").read_text()
    msc = (ROOT / "firmware/src/usb/msc.c").read_text()
    connected = (function_source(host, "pf_sdio_pio_host_forget_removed_card") + "\n" +
                 function_source(card, "pf_sdio_pio_confirm_card_removed") + "\n" +
                 "\n".join(function_source(facade, name) for name in (
                     "pf_sd_card_get_owner", "pf_sd_card_msc_acquire",
                     "pf_sd_card_msc_release",
                     "pf_sd_card_take_reinit_request", "pf_sd_card_confirm_removed")))
    connected = connected.replace("reinit_after_usb_owner", "reinit_request")
    usb = "static bool transfer_sense_pending;\n" + "\n".join(function_source(msc, name) for name in (
        "set_sense",
        "pf_usb_msc_request", "apply_msc_request", "pf_usb_msc_task", "tud_msc_start_stop_cb",
        "tud_msc_prevent_allow_medium_removal_cb"))
    harness = (HARNESS.replace("@STATE@", defines + "\n" + state)
               .replace("@ATTEMPT@", function_source(source, "attempt_auto_mount"))
               .replace("@FUNCTION@", function_source(source, "service_card_presence") + "\n" +
                        function_source(source, "pf_storage_worker_media_absent"))
               .replace("@FACADE@", connected).replace("@MSC@", usb))
    with tempfile.TemporaryDirectory() as directory:
        test_source = Path(directory) / "automount.c"
        executable = Path(directory) / "automount"
        test_source.write_text(harness)
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                        "-Werror", str(test_source), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()