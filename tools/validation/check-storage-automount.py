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
    start = source.index("static void " + name + "(void)")
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
static owner_t owner = PF_SD_CARD_OWNER_FIRMWARE;
static pf_sd_card_info_t media = { true, true, false };
static uint32_t now;
static bool mounted, reinit_request;
static unsigned mount_calls, unmount_calls, forget_calls;
static pf_storage_result_t mount_result = PF_STORAGE_OK;
static owner_t pf_sd_card_get_owner(void) { return owner; }
static void pf_sd_card_get_info(pf_sd_card_info_t *info) { *info = media; }
static bool pf_storage_is_mounted(void) { return mounted; }
static pf_storage_result_t pf_storage_unmount(void *result)
{
    (void)result;
    ++unmount_calls;
    mounted = false;
    return PF_STORAGE_OK;
}
static void pf_storage_forget_filesystem_format(void) { ++forget_calls; }
static bool pf_sd_card_is_initialized(void) { return media.initialized; }
static bool pf_sd_card_take_reinit_request(void)
{
    bool requested = reinit_request;
    reinit_request = false;
    return requested;
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
    poll(3);
    assert(forget_calls == 1);
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
    owner = PF_SD_CARD_OWNER_USB_MSC;
    poll(60);
    assert(mount_calls == 4 && reinit_request);
    owner = PF_SD_CARD_OWNER_FIRMWARE;
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
    puts("Storage auto-mount checks passed");
}
"""


def main():
    source = WORKER.read_text()
    state = source[source.index("static bool presence_sample_valid;"):
                   source.index("static void refresh_self_test_completion")]
    defines = "\n".join(re.findall(r"^#define CARD_.*$", source, re.MULTILINE))
    harness = (HARNESS.replace("@STATE@", defines + "\n" + state)
               .replace("@ATTEMPT@", function_source(source, "attempt_auto_mount"))
               .replace("@FUNCTION@", function_source(source, "service_card_presence")))
    with tempfile.TemporaryDirectory() as directory:
        test_source = Path(directory) / "automount.c"
        executable = Path(directory) / "automount"
        test_source.write_text(harness)
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                        "-Werror", str(test_source), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()