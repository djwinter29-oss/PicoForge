// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

/**
 * @file
 * @brief Storage command group and worker-side volume maintenance.
 */
#include "cli_storage.h"
#include "cli.h"
#include "cli_fs.h"
#include "cli_fatfs_result.h"
#include "cli_internal.h"
#include "app/usb/usb_msc.h"
#include "platform/storage/fatfs_volume.h"
#include "platform/storage/storage.h"
#include "embedded_cli.h"

#include <stdio.h>
#include <string.h>

/** Element count of a true array. */
#define PF_CLI_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

static struct {
    pf_cli_worker_submit_fn_t submit; /**< Borrowed worker submission callback. */
    void *context; /**< Borrowed callback context. */
} storage_dispatch;

/** @brief Name for an enum value, or "unknown" when it is outside the table. */
static const char *name_or_unknown(const char *const *names, size_t count, unsigned index)
{
    return index < count ? names[index] : "unknown";
}

/** @brief Report the backend-neutral storage status on one line. */
static void status_command(bool host_owned, pf_storage_worker_reply_fn_t reply, void *context)
{
    static const char *const states[] = {
        "not_configured", "busy", "absent", "uninitialized", "ready", "mounted", "usb",
    };
    static const char *const operations[] = {"none", "init", "read", "write", "sync", "msc_acquire"};
    pf_storage_status_t status;
    char line[PF_STORAGE_WORKER_REPLY_SIZE];

    pf_storage_get_status(&status);
    (void)snprintf(line, sizeof(line),
                   "state=%s owner=%s medium=%s bus=%s present=%s clock=%lu Hz sectors=%lu bytes=%llu "
                   "last_error=%s:%u",
                   name_or_unknown(states, PF_CLI_ARRAY_SIZE(states), (unsigned)status.state),
                   host_owned ? "USB" : "Pico",
                   status.medium == PF_STORAGE_MEDIUM_SDCARD ? "sd" : "none",
                   status.bus == PF_STORAGE_BUS_SPI ? "spi" :
                   status.bus == PF_STORAGE_BUS_SDIO ? "sdio" : "unknown",
                   status.state == PF_STORAGE_STATE_ABSENT ? "no" : status.presence_known ? "yes" : "unknown",
                   (unsigned long)status.bus_clock_hz, (unsigned long)status.sector_count,
                   (unsigned long long)status.sector_count * FF_MAX_SS,
                   name_or_unknown(operations, PF_CLI_ARRAY_SIZE(operations), (unsigned)status.last_failure),
                   (unsigned)status.last_failure_result);
    (void)reply(line, context);
}

/** @brief Mount or format; the caller verified Pico ownership. */
static void mount_or_format(bool formatting, pf_storage_worker_reply_fn_t reply, void *context)
{
    const pf_block_device_t *device;
    FRESULT result;
    pf_fatfs_volume_result_t storage_result;

    if (pf_fatfs_volume_is_mounted()) {
        (void)reply(formatting ? "ERR Unmount first with storage unmount; format refused." :
                                "OK Filesystem already mounted.",
                    context);
        return;
    }
    if (pf_storage_initialize() != PF_BLOCK_OK) {
        (void)reply("ERR Mount failed: storage initialization or board configuration unavailable.", context);
        return;
    }
    device = pf_storage_firmware_device();
    if (formatting) {
        result = pf_fatfs_volume_format(device);
        if (result != FR_OK) {
            (void)reply("WARN Format failed; volume contents may be partially changed.", context);
            pf_cli_report_result(result, reply, context);
            return;
        }
        (void)reply("OK Filesystem formatted; mounting.", context);
    }
    storage_result = pf_fatfs_volume_mount(device, &result);
    if (storage_result == PF_FATFS_VOLUME_OK) {
        (void)reply("OK Filesystem mounted.", context);
    } else if (storage_result == PF_FATFS_VOLUME_ALREADY_MOUNTED) {
        (void)reply("OK Filesystem already mounted.", context);
    } else if (storage_result == PF_FATFS_VOLUME_FILESYSTEM_ERROR) {
        pf_cli_report_result(result, reply, context);
    } else {
        (void)reply("ERR Mount failed: invalid block-device descriptor.", context);
    }
}

/** @brief Unmount the volume, allowing MSC handoff. */
static void unmount_command(pf_storage_worker_reply_fn_t reply, void *context)
{
    FRESULT result;

    if (pf_fatfs_volume_unmount(&result) == PF_FATFS_VOLUME_OK) {
        (void)reply("OK Filesystem unmounted; MSC handoff is now allowed.", context);
    } else {
        pf_cli_report_result(result, reply, context);
    }
}

/** @brief Report USB mass-storage ownership; requests complete asynchronously. */
static void print_usb(void)
{
    char line[80];
    snprintf(line, sizeof(line), "usb=%s ejected=%u pending=%u rejected=%u",
             pf_usb_msc_is_enabled() ? "enabled" : "disabled", pf_usb_msc_is_ejected(),
             pf_usb_msc_transition_pending(), pf_usb_msc_request_rejected());
    pf_cli_print(line);
}

/** @brief Queue a normal MSC ownership request, never bypassing clean host eject. */
static void set_usb(bool enabled)
{
    if (pf_storage_worker_busy() || pf_usb_msc_transition_pending()) {
        pf_cli_print("ERR storage busy");
    } else if (!pf_usb_msc_request(enabled)) {
        pf_cli_print("ERR USB request refused");
    } else {
        pf_cli_print("QUEUED USB ownership request; check storage get usb for completion/rejection");
    }
}

#if PF_ENABLE_STORAGE_BENCHMARK
/** @brief Format the retained benchmark snapshot, including admission and cleanup failures. */
static void print_benchmark(void)
{
    static const char *const states[] = {
        "idle", "queued", "preparing", "writing", "reading", "cleanup", "passed", "failed", "cancelled"
    };
    pf_file_benchmark_status_t status;
    char line[160];
    pf_storage_worker_benchmark_status(&status);
    const char *state = (unsigned)status.state < sizeof(states) / sizeof(states[0]) ? states[status.state] : "unknown";
    snprintf(line, sizeof(line), "benchmark=%s target=%lu written=%lu read=%lu mismatches=%lu", state,
             (unsigned long)status.target_bytes, (unsigned long)status.written_bytes, (unsigned long)status.read_bytes,
             (unsigned long)status.mismatch_count);
    pf_cli_print(line);
    snprintf(line, sizeof(line), "write_ms=%lu read_ms=%lu error=%u filesystem_result=%u request_error=%u flags=%u",
             (unsigned long)status.write_elapsed_ms, (unsigned long)status.read_elapsed_ms,
             status.error, status.filesystem_result, status.request_error, status.flags);
    pf_cli_print(line);
}
#endif

/** @brief Start or cancel through worker admission; `storage get benchmark` reports asynchronous outcomes. */
static void benchmark_request(const char *value)
{
#if PF_ENABLE_STORAGE_BENCHMARK
    if (strcmp(value, "cancel") == 0) {
        pf_cli_print(pf_storage_worker_benchmark_cancel() ? "QUEUED benchmark cancellation; wait for cleanup" :
                                                          "ERR no cancellable run or admission/cleanup busy");
        return;
    }
    uint32_t size = 0u;
    for (const char *position = value; *position != '\0'; ++position) {
        if (*position < '0' || *position > '9' || size > PF_FILE_BENCHMARK_MAX_MIB) {
            pf_cli_print("ERR benchmark size must be 1..1024 MiB");
            return;
        }
        size = size * 10u + (unsigned)(*position - '0');
    }
    if (size == 0u || size > PF_FILE_BENCHMARK_MAX_MIB) {
        pf_cli_print("ERR benchmark size must be 1..1024 MiB");
    } else if (pf_usb_msc_is_enabled() || pf_usb_msc_transition_pending()) {
        pf_cli_print("ERR USB owns storage or ownership transition pending");
    } else if (pf_storage_worker_benchmark_start(size, false)) {
        pf_cli_print("QUEUED benchmark request; preflight is pending, check storage get benchmark");
    } else {
        pf_cli_print("ERR benchmark request refused");
        print_benchmark();
    }
#else
    (void)value;
    pf_cli_print("ERR benchmark unavailable in this build");
#endif
}

/** @brief Handle synchronous `storage` verbs; return false when execution belongs on the worker. */
static bool handle_storage_command(const char *args)
{
    char tokens[PF_CLI_STORAGE_ARGS_SIZE] = { 0 };
    const size_t length = args != NULL ? strlen(args) : 0u;
    if (length > sizeof(tokens) - 2u) {
        pf_cli_print("ERR storage arguments too long");
        return true;
    }
    if (length > 0u) memcpy(tokens, args, length);
    embeddedCliTokenizeArgs(tokens);
    const uint16_t count = embeddedCliGetTokenCount(tokens);
    const char *verb = count > 0u ? embeddedCliGetToken(tokens, 1u) : "";
    const char *item = count > 1u ? embeddedCliGetToken(tokens, 2u) : "";
    const char *value = count > 2u ? embeddedCliGetToken(tokens, 3u) : "";

    if ((strcmp(verb, "get") == 0 && (count == 1u || (count == 2u && strcmp(item, "status") == 0))) ||
        (count == 1u && (strcmp(verb, "mount") == 0 || strcmp(verb, "unmount") == 0)) ||
        (count == 2u && strcmp(verb, "format") == 0 && strcmp(item, "--confirm") == 0)) {
        return false;
    }
    if (count == 2u && strcmp(verb, "get") == 0 && strcmp(item, "usb") == 0) {
        print_usb();
    } else if (count == 2u && strcmp(verb, "get") == 0 && strcmp(item, "benchmark") == 0) {
#if PF_ENABLE_STORAGE_BENCHMARK
        print_benchmark();
#else
        pf_cli_print("ERR benchmark unavailable in this build");
#endif
    } else if (count == 3u && strcmp(verb, "set") == 0 && strcmp(item, "usb") == 0 &&
               (strcmp(value, "on") == 0 || strcmp(value, "off") == 0)) {
        set_usb(strcmp(value, "on") == 0);
    } else if (count == 2u && strcmp(verb, "benchmark") == 0) {
        benchmark_request(item);
    } else if (count == 2u && strcmp(verb, "recover") == 0 && strcmp(item, "--confirm") == 0) {
        if (!pf_usb_msc_request_recovery()) {
            pf_cli_print("ERR fault recovery request refused; check storage get usb");
            return true;
        }
        pf_cli_print("QUEUED fault recovery; not a clean eject. Data may be lost; inspect before storage mount. "
                     "Check UART recovery result and fresh storage get status.");
    } else {
        pf_cli_print("ERR storage " PF_CLI_STORAGE_HELP);
    }
    return true;
}

static void storage_binding_command(EmbeddedCli *instance, char *args, void *context)
{
    (void)instance;
    (void)context;
    if (handle_storage_command(args)) return;
    if (!storage_dispatch.submit("storage", args != NULL ? args : "", storage_dispatch.context)) {
        pf_cli_print("ERR Storage busy or command too long; wait for completion and retry.");
    } else {
        pf_cli_print("QUEUED Storage command; check the result below.");
    }
}

bool pf_cli_storage_register(pf_cli_worker_submit_fn_t submit, void *context)
{
    if (submit == NULL) return false;
    storage_dispatch.submit = submit;
    storage_dispatch.context = context;
    return pf_cli_add_binding((CliCommandBinding){
        "storage", PF_CLI_STORAGE_HELP, false, NULL, storage_binding_command
    });
}

void pf_cli_storage_execute(bool host_owned, const char *family, const char *arguments,
                            pf_storage_worker_reply_fn_t reply, void *context)
{
    char tokens[PF_STORAGE_WORKER_ARGS_SIZE] = { 0 };
    const char *operation; /* host_owned is the caller's snapshot; see the header for the no-transition rule. */
    uint16_t count;
    bool formatting;
    bool maintenance;

    if ((reply == NULL) || (family == NULL) || (arguments == NULL)) {
        return;
    }
    if (strlen(arguments) > sizeof(tokens) - 2u) {
        (void)reply("ERR Arguments too long.", context);
        return;
    }
    memcpy(tokens, arguments, strlen(arguments));
    embeddedCliTokenizeArgs(tokens);
    count = embeddedCliGetTokenCount(tokens);
    if (count == 0u) {
        (void)reply("ERR Usage: storage get [status] | storage <mount|unmount|format --confirm> | fs <verb> [args]",
                    context);
        return;
    }
    operation = embeddedCliGetToken(tokens, 1u);
    if ((strcmp(family, "storage") == 0) && (strcmp(operation, "get") == 0) &&
        ((count == 1u) || ((count == 2u) && (strcmp(embeddedCliGetToken(tokens, 2u), "status") == 0)))) {
        status_command(host_owned, reply, context);
        return;
    }
    maintenance = (strcmp(family, "storage") == 0) &&
                  (strcmp(operation, "mount") == 0 || strcmp(operation, "unmount") == 0 ||
                   strcmp(operation, "format") == 0);
    if (!maintenance && strcmp(family, "fs") != 0) {
        (void)reply("ERR Usage: storage get [status] | storage <mount|unmount|format --confirm> | fs <verb> [args]",
                    context);
        return;
    }
    if (!maintenance && (strcmp(operation, "mount") == 0 || strcmp(operation, "unmount") == 0 ||
                         strcmp(operation, "format") == 0)) {
        (void)reply("ERR Volume maintenance moved to storage. Use help storage.", context);
        return;
    }
    if (host_owned) {
        (void)reply("ERR Media owned by USB. Safely eject and disable MSC first.", context);
        return;
    }
    formatting = maintenance && strcmp(operation, "format") == 0;
    if (formatting && ((count != 2u) || (strcmp(embeddedCliGetToken(tokens, 2u), "--confirm") != 0))) {
        (void)reply("ERR Usage: storage format --confirm (destroys all files on selected logical volume)", context);
        return;
    }
    if (maintenance && !formatting && count != 1u) {
        (void)reply("ERR Invalid arguments. Use help storage.", context);
        return;
    }
    if (formatting || (maintenance && strcmp(operation, "mount") == 0)) {
        mount_or_format(formatting, reply, context);
        return;
    }
    if (!pf_fatfs_volume_is_mounted()) {
        (void)reply("ERR Filesystem not mounted. Run storage mount first.", context);
    } else if (maintenance && strcmp(operation, "unmount") == 0) {
        unmount_command(reply, context);
    } else {
        pf_cli_fs_execute(operation, count, embeddedCliGetToken(tokens, 2u), embeddedCliGetToken(tokens, 3u),
                          reply, context);
    }
}
