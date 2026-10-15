#!/usr/bin/env python3
"""Check production HID/start paths while the card reservation is unavailable."""
import os
from importlib.machinery import SourceFileLoader
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
function_source = SourceFileLoader(
    "automount", str(ROOT / "tools/validation/check-storage-automount.py")
).load_module().function_source

HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include "usb/hid.h"
#include "board/board.h"
#include "middleware/storage/storage_worker.h"

#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define pdPASS 1
typedef struct { uint32_t test_size_mib; } storage_request_t;
static bool busy, card_service_busy, config_available = true;
static bool self_test_active, self_test_finished, self_test_cancelled;
static pf_self_test_status_t self_test_status;
static void *requests = (void *)1;
static bool unavailable, reserved, initialized, mounted, queue_fails;
static unsigned queries, queued;
static void refresh_self_test_completion(void) {}
bool pf_sd_card_try_lock(void)
{
    assert(!reserved);
    reserved = !unavailable;
    return reserved;
}
void pf_sd_card_unlock(void) { assert(reserved); reserved = false; }
bool pf_sd_card_is_initialized(void) { assert(reserved); return initialized; }
bool pf_storage_is_mounted(void) { assert(reserved); return mounted; }
void pf_sd_card_get_info(pf_sd_card_info_t *info)
{
    assert(reserved);
    ++queries;
    *info = (pf_sd_card_info_t){
        .initialized = initialized, .presence_known = true, .present = true,
        .cid_valid = initialized, .bus_clock_hz = 25000000, .block_count = 128
    };
}
bool pf_board_get_sd_card_config(pf_sd_card_config_t *config)
{
    config->transport = PF_SD_CARD_TRANSPORT_SDIO_PIO;
    return true;
}
static int xQueueSend(void *queue, const storage_request_t *request, unsigned timeout)
{
    assert(queue == requests && request->test_size_mib == 32 && timeout == 0);
    if (queue_fails) return 0;
    ++queued;
    return pdPASS;
}
@MEMORY@
@START@

typedef enum { HID_REPORT_TYPE_INPUT = 1, HID_REPORT_TYPE_FEATURE = 3 } hid_report_type_t;
void pf_hid_build_info_report(pf_hid_info_report_t *report) { memset(report, 0, sizeof(*report)); }
void pf_hid_build_status_report(pf_hid_status_report_t *report, uint8_t sequence)
{ memset(report, 0, sizeof(*report)); report->sequence = sequence; }
void pf_hid_build_msc_status_report(pf_hid_msc_status_report_t *report)
{ memset(report, 0, sizeof(*report)); }
void pf_hid_build_self_test_report(pf_hid_self_test_status_t *report, uint8_t sequence)
{ memset(report, 0, sizeof(*report)); report->sequence = sequence; }
@GET_REPORT@

typedef struct EmbeddedCli EmbeddedCli;
static uint32_t cli_session = 9;
static unsigned recovery_requests, normal_submissions, messages;
static void pf_usb_msc_request_recovery(void) { ++recovery_requests; }
static bool pf_usb_msc_is_enabled(void) { return false; }
static void pf_cli_print(const char *message) { assert(message && *message); ++messages; }
bool pf_storage_worker_submit(const char *family, const char *arguments,
                              bool host_owned, uint32_t session)
{
    assert(family && arguments && !host_owned && session == cli_session);
    ++normal_submissions;
    return true;
}
@COMMAND@

int main(void)
{
    pf_hid_memory_report_t report, sentinel;
    memset(&sentinel, 0xa5, sizeof(sentinel));
    report = sentinel;
    unavailable = true;
    unsigned usb_iterations = 0;
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        assert(!pf_hid_build_memory_report(&report, 7));
        assert(!memcmp(&report, &sentinel, sizeof(report)));
        assert(!pf_storage_worker_self_test_start(32, false));
        assert(self_test_status.request_error == PF_SELF_TEST_ERROR_BUSY);
        assert(!busy && !self_test_active && !reserved && !queries && !queued);
        uint8_t payload[sizeof(pf_hid_memory_report_t)];
        memset(payload, 0xa5, sizeof(payload));
        assert(tud_hid_get_report_cb(0, PF_HID_MEMORY_REPORT_ID,
                                    HID_REPORT_TYPE_INPUT, payload, sizeof(payload)) == 0);
        for (unsigned i = 0; i < sizeof(payload); ++i) assert(payload[i] == 0xa5);
        assert(tud_hid_get_report_cb(0, PF_HID_STATUS_REPORT_ID, HID_REPORT_TYPE_INPUT,
                                    payload, sizeof(payload)) == sizeof(pf_hid_status_report_t));
        ++usb_iterations;
    }
    assert(usb_iterations == 1000);
    unavailable = false;
    assert(pf_hid_build_memory_report(&report, 7));
    assert(report.version == PF_HID_REPORT_VERSION && report.sequence == 7);
    assert(!(report.flags & PF_HID_MEMORY_INITIALIZED));
    assert(!pf_storage_worker_self_test_start(32, false));
    assert(self_test_status.request_error == PF_SELF_TEST_ERROR_CARD && !reserved);
    initialized = true;
    assert(pf_hid_build_memory_report(&report, 8));
    assert(report.flags & PF_HID_MEMORY_INITIALIZED);
    assert(report.bus_clock_hz == 25000000 && report.block_count == 128);
    uint8_t payload[sizeof(report)];
    assert(tud_hid_get_report_cb(0, PF_HID_MEMORY_REPORT_ID, HID_REPORT_TYPE_INPUT,
                                payload, sizeof(payload)) == sizeof(report));
    assert(!pf_storage_worker_self_test_start(32, false));
    assert(self_test_status.request_error == PF_SELF_TEST_ERROR_NOT_MOUNTED && !reserved);
    mounted = true;
    assert(!pf_storage_worker_self_test_start(32, true));
    assert(self_test_status.request_error == PF_SELF_TEST_ERROR_USB_OWNER);
    assert(!pf_storage_worker_self_test_start(0, false));
    assert(self_test_status.request_error == PF_SELF_TEST_ERROR_SIZE);
    queue_fails = true;
    assert(!pf_storage_worker_self_test_start(32, false));
    assert(!busy && !self_test_active);
    queue_fails = false;
    assert(pf_storage_worker_self_test_start(32, false));
    assert(busy && self_test_active && queued == 1);
    assert(self_test_status.state == PF_SELF_TEST_QUEUED);
    assert(!pf_storage_worker_self_test_start(32, false));
    assert(self_test_status.request_error == PF_SELF_TEST_ERROR_BUSY && queued == 1);
    char recovery[] = "recover --confirm", unconfirmed[] = "recover", status[] = "status";
    storage_command(NULL, recovery, "sd");
    assert(recovery_requests == 1 && normal_submissions == 0 && messages == 1);
    storage_command(NULL, unconfirmed, "sd");
    storage_command(NULL, status, "sd");
    storage_command(NULL, recovery, "fs");
    assert(recovery_requests == 1 && normal_submissions == 3 && messages == 4);
}
"""


def main():
    hid = (ROOT / "firmware/src/usb/hid.c").read_text()
    worker = (ROOT / "firmware/src/middleware/storage/storage_worker.c").read_text()
    code = HARNESS.replace("@MEMORY@", function_source(hid, "pf_hid_build_memory_report"))
    code = code.replace("@START@", function_source(worker, "pf_storage_worker_self_test_start"))
    code = code.replace("@GET_REPORT@", function_source(hid, "tud_hid_get_report_cb"))
    task = (ROOT / "firmware/src/usb/task.c").read_text()
    code = code.replace("@COMMAND@", function_source(task, "storage_command"))
    with tempfile.TemporaryDirectory() as directory:
        scratch = Path(directory)
        for name, body in {
            "hardware/pio.h": "typedef void *PIO;\n",
            "hardware/spi.h": "typedef struct spi_inst spi_inst_t;\n",
            "pico/unique_id.h": "#define PICO_UNIQUE_BOARD_ID_SIZE_BYTES 8u\n",
        }.items():
            header = scratch / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text(body)
        source = scratch / "test.c"
        exe = scratch / "test"
        source.write_text(code)
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I" + directory, "-I" + str(ROOT / "firmware/src"),
            "-I" + str(ROOT / "firmware/src/drivers/sd_card"),
            "-I" + str(ROOT / "firmware/third_party/fatfs/source"),
            "-I" + str(ROOT / "firmware/src/integration/fatfs"),
            str(source), "-o", str(exe)
        ], check=True)
        subprocess.run([str(exe)], check=True, timeout=10)
    print("Production HID/start paths: busy-card nonblocking and readiness/queue checks passed")


if __name__ == "__main__":
    main()
