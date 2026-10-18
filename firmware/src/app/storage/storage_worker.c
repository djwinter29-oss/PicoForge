/** @file Application storage-owner task, command sessions, and media policy. */
#include "storage_worker.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "drivers/system.h"
#include "app/storage/fatfs_volume.h"
#include "middleware/logging/logging.h"
#if PF_STORAGE_BACKEND_NOR
#include "nor_media.h"
#else
#include "sd_media.h"
#endif

#include <string.h>
#include <stdatomic.h>

#define STORAGE_WORKER_STACK_SIZE 2048u
#define STORAGE_REPLY_COUNT 8u
#define MEDIA_SERVICE_POLL_MS 100u
#define CARD_PRESENCE_DEBOUNCE_SAMPLES 3u
#define MEDIA_MOUNT_RETRY_MS 5000u

/** One admitted operation; only one command, benchmark or recovery may be outstanding. */
typedef struct {
    uint32_t session; /**< Originating command session. */
    uint32_t test_size_mib; /**< Nonzero selects a file benchmark. */
    bool host_owned; /**< Ownership snapshot protected by the admission gate. */
    bool recover_media; /**< Selects explicit recovery instead of a command. */
    char family[3]; /**< Command family: sd or fs. */
    char arguments[PF_STORAGE_COMMAND_ARGS_SIZE]; /**< Copied command arguments. */
} storage_request_t;

static StaticTask_t worker_control;
static StackType_t worker_stack[STORAGE_WORKER_STACK_SIZE];
static StaticQueue_t request_control;
static StaticQueue_t reply_control;
static uint8_t request_storage[sizeof(storage_request_t)];
static uint8_t reply_storage[STORAGE_REPLY_COUNT * sizeof(pf_storage_worker_reply_t)];
static QueueHandle_t requests;
static QueueHandle_t replies;
static bool config_available;
static bool busy;
static uint32_t active_session;
/* Request admission/completion and self-test publication use critical sections.
 * The worker reads cancellation while waiting for replies; atomic publication
 * avoids relying on volatile for cross-task communication. Presence/automount
 * policy is worker-owned and published while media_service_busy is reserved. */
static atomic_bool cancelled;
static pf_file_benchmark_status_t benchmark_status;
static bool benchmark_active;
static bool benchmark_finished;
static bool benchmark_cancelled;
static bool media_service_busy;
#if !PF_STORAGE_BACKEND_NOR
static bool presence_sample_valid;
static bool presence_sample;
static uint8_t presence_sample_count;
static bool stable_presence_valid;
static bool stable_card_present;
#endif
static bool auto_mount_pending = true;
static bool auto_mount_blocked;
static bool mount_retry_waiting;
static uint32_t last_mount_attempt_ms;
static bool mount_failure_reported; /**< Suppress identical periodic mount failures until success. */
static unsigned last_media_result; /**< Last reported selected-backend initialization outcome. */
static pf_fatfs_volume_result_t last_volume_result; /**< Last reported mount outcome. */
static FRESULT last_filesystem_result; /**< Last reported FatFs mount reason. */

static void refresh_benchmark_completion(void)
{
    taskENTER_CRITICAL();
    if (benchmark_active && benchmark_finished) {
        benchmark_active = false;
        benchmark_finished = false;
        busy = false;
    }
    taskEXIT_CRITICAL();
}

static uint32_t benchmark_clock(void *context)
{
    (void)context;
    return pf_system_uptime_ms();
}

static bool benchmark_update(const pf_file_benchmark_status_t *status, void *context)
{
    (void)context;
    taskENTER_CRITICAL();
    pf_file_benchmark_error_t request_error = benchmark_status.request_error;
    benchmark_status = *status;
    benchmark_status.request_error = request_error;
    bool keep_running = !benchmark_cancelled;
    taskEXIT_CRITICAL();
    return keep_running;
}

static bool send_reply(const char *line, void *context)
{
    const storage_request_t *request = context;
    pf_storage_worker_reply_t reply = { .session = request->session };

    if (strlen(line) >= sizeof(reply.line)) {
        return false;
    }
    memcpy(reply.line, line, strlen(line) + 1u);
    while (!cancelled) {
        if (xQueueSend(replies, &reply, pdMS_TO_TICKS(50u)) == pdPASS) {
            return true;
        }
    }
    return false;
}

/** @brief Attempt one mount and retain/report changed failures without flooding paced retries. */
static void attempt_auto_mount(void)
{
    unsigned media_result = 0u;
    pf_fatfs_volume_result_t volume_result = PF_FATFS_VOLUME_OK;
    FRESULT filesystem_result = FR_OK;
    if (pf_fatfs_volume_is_mounted()) {
        auto_mount_pending = false;
    } else {
#if PF_STORAGE_BACKEND_NOR
        media_result = pf_nor_media_initialize();
        bool media_ready = media_result == PF_BLOCK_OK;
#else
        media_result = (unsigned)pf_sd_media_initialize();
        bool media_ready = media_result == PF_SD_OK;
#endif
        if (!media_ready) {
            auto_mount_pending = true;
        } else {
#if PF_STORAGE_BACKEND_NOR
            volume_result = pf_fatfs_volume_mount(pf_nor_media_firmware_device(), &filesystem_result);
#else
            volume_result = pf_fatfs_volume_mount(pf_sd_media_firmware_device(), &filesystem_result);
#endif
            auto_mount_pending = (volume_result != PF_FATFS_VOLUME_OK) &&
                                 (volume_result != PF_FATFS_VOLUME_ALREADY_MOUNTED);
        }
    }
    if (auto_mount_pending) {
        if (!mount_failure_reported || media_result != last_media_result ||
            volume_result != last_volume_result || filesystem_result != last_filesystem_result) {
            PF_LOG_WARNING("storage", "Automount failed: media=%u volume=%u FatFs=%u; retry in %u ms",
                           (unsigned)media_result, (unsigned)volume_result,
                           (unsigned)filesystem_result, MEDIA_MOUNT_RETRY_MS);
        }
        mount_failure_reported = true;
        last_media_result = media_result;
        last_volume_result = volume_result;
        last_filesystem_result = filesystem_result;
    } else {
        mount_failure_reported = false;
    }
    last_mount_attempt_ms = pf_system_uptime_ms();
    mount_retry_waiting = auto_mount_pending;
}

/** @brief Service idle-only lifecycle policy while excluding worker admission and MSC transitions. */
static void service_media(void)
{
#if PF_STORAGE_BACKEND_NOR
    taskENTER_CRITICAL();
    if (busy || media_service_busy) {
        taskEXIT_CRITICAL();
        return;
    }
    media_service_busy = true;
    taskEXIT_CRITICAL();
    pf_nor_media_lock();
    if (!pf_nor_media_usb_owned() && pf_fatfs_volume_is_mounted() && !pf_nor_media_ready()) {
        if (pf_fatfs_volume_unmount(NULL) != PF_FATFS_VOLUME_OK) {
            goto nor_done;
        }
        auto_mount_pending = true;
        mount_retry_waiting = false;
    }
    if (pf_nor_media_take_remount_request()) {
        auto_mount_pending = true;
        mount_retry_waiting = false;
    }
    if (!pf_nor_media_usb_owned() && !auto_mount_blocked && auto_mount_pending &&
        (!mount_retry_waiting ||
         (uint32_t)(pf_system_uptime_ms() - last_mount_attempt_ms) >= MEDIA_MOUNT_RETRY_MS)) {
        attempt_auto_mount();
    }
nor_done:
    pf_nor_media_unlock();
    taskENTER_CRITICAL();
    media_service_busy = false;
    taskEXIT_CRITICAL();
#else
    pf_sd_info_t info;

    if (!config_available) {
        return;
    }
    taskENTER_CRITICAL();
    if (busy || media_service_busy) {
        taskEXIT_CRITICAL();
        return;
    }
    media_service_busy = true;
    taskEXIT_CRITICAL();

    pf_sd_lock();
    pf_sd_get_info(&info);
    bool firmware_owned = pf_sd_media_get_owner() == PF_SD_MEDIA_OWNER_FIRMWARE;
    bool mounted_card_uninitialized = pf_fatfs_volume_is_mounted() && !info.initialized;
    if (firmware_owned && mounted_card_uninitialized && (!info.presence_known || info.present)) {
        if (pf_fatfs_volume_unmount(NULL) != PF_FATFS_VOLUME_OK) {
            goto done;
        }
        pf_fatfs_volume_forget_filesystem_format();
        auto_mount_pending = true;
        mount_retry_waiting = false;
    }
    if (info.presence_known) {
        if (!presence_sample_valid || (info.present != presence_sample)) {
            presence_sample = info.present;
            presence_sample_count = 1u;
            presence_sample_valid = true;
        } else if (presence_sample_count < CARD_PRESENCE_DEBOUNCE_SAMPLES) {
            ++presence_sample_count;
        }
        if (presence_sample_count < CARD_PRESENCE_DEBOUNCE_SAMPLES) {
            goto done;
        }

        if (!stable_presence_valid || (stable_card_present != presence_sample)) {
            stable_presence_valid = true;
            stable_card_present = presence_sample;
            auto_mount_pending = true;
            mount_retry_waiting = false;
            if (!stable_card_present) {
                pf_sd_confirm_removed();
                if (pf_fatfs_volume_is_mounted()) {
                    (void)pf_fatfs_volume_unmount(NULL);
                }
                pf_fatfs_volume_forget_filesystem_format();
                goto done;
            }
        }

        if (!stable_card_present) {
            goto done;
        }
    }
    if (!firmware_owned) {
        goto done;
    }
    if (auto_mount_blocked) {
        goto done;
    }
    if (pf_sd_media_take_reinit_request()) {
        auto_mount_pending = true;
        mount_retry_waiting = false;
    }
    if (pf_fatfs_volume_is_mounted()) {
        auto_mount_pending = false;
        goto done;
    }
    if (!auto_mount_pending) {
        goto done;
    }
    if (mount_retry_waiting &&
        ((uint32_t)(pf_system_uptime_ms() - last_mount_attempt_ms) < MEDIA_MOUNT_RETRY_MS)) {
        goto done;
    }
    attempt_auto_mount();

done:
    pf_sd_unlock();
    taskENTER_CRITICAL();
    media_service_busy = false;
    taskEXIT_CRITICAL();
#endif
}

/** @brief Recover only after USB handoff; leave automatic mounting blocked for explicit inspection. */
static void recover_media(void)
{
    auto_mount_blocked = true;
    auto_mount_pending = false;
#if PF_STORAGE_BACKEND_NOR
    pf_block_result_t result = pf_nor_media_initialize();
    bool recovered = result == PF_BLOCK_OK;
#else
    pf_sd_lock();
    (void)pf_sd_media_take_reinit_request();
    pf_sd_result_t result = pf_sd_media_initialize();
    bool recovered = result == PF_SD_OK;
    pf_sd_unlock();
#endif
    if (recovered) {
        PF_LOG_INFO("storage", "Media recovery completed; volume remains unmounted pending explicit fs mount");
    } else {
        PF_LOG_ERROR("storage", "Media recovery failed: result=%u; volume remains unmounted", (unsigned)result);
    }
    taskENTER_CRITICAL();
    busy = false;
    taskEXIT_CRITICAL();
}

/** @brief Execute a command and publish completion before the USB task releases admission. */
static void execute_command(storage_request_t *request)
{
    bool previously_mounted = pf_fatfs_volume_is_mounted();
    pf_storage_command_execute(request->host_owned, request->family, request->arguments,
                               send_reply, request);
    if (previously_mounted && !pf_fatfs_volume_is_mounted()) {
        auto_mount_pending = false;
    }
    if (pf_fatfs_volume_is_mounted()) {
        auto_mount_blocked = false;
        mount_failure_reported = false;
    }
    pf_storage_worker_reply_t reply = { .session = request->session, .done = true };
    (void)xQueueSend(replies, &reply, portMAX_DELAY);
}

static void storage_task(void *context)
{
    (void)context;
    storage_request_t request;

    for (;;) {
        if (xQueueReceive(requests, &request,
                          pdMS_TO_TICKS(MEDIA_SERVICE_POLL_MS)) != pdPASS) {
            service_media();
            continue;
        }
        if (request.test_size_mib != 0u) {
            pf_file_benchmark_run(request.test_size_mib,
                                  benchmark_clock, benchmark_update, NULL);
            taskENTER_CRITICAL();
            benchmark_finished = true;
            taskEXIT_CRITICAL();
            continue;
        }
        if (request.recover_media) {
            recover_media();
            continue;
        }
        execute_command(&request);
    }
}

bool pf_storage_worker_init(void)
{
    if (requests != NULL) {
        return false;
    }
#if PF_STORAGE_BACKEND_NOR
    if (!pf_nor_media_setup()) {
        return false;
    }
    config_available = true;
#else
    pf_sd_result_t setup_result = pf_sd_media_setup();
    if (setup_result != PF_SD_OK && setup_result != PF_SD_NOT_CONFIGURED) {
        return false;
    }
    config_available = setup_result == PF_SD_OK;
#endif
    requests = xQueueCreateStatic(1u, sizeof(storage_request_t), request_storage, &request_control);
    replies = xQueueCreateStatic(STORAGE_REPLY_COUNT, sizeof(pf_storage_worker_reply_t),
                                 reply_storage, &reply_control);
    return (requests != NULL) && (replies != NULL) &&
           (xTaskCreateStatic(storage_task, "Storage", STORAGE_WORKER_STACK_SIZE, NULL,
                              tskIDLE_PRIORITY + 1u, worker_stack, &worker_control) != NULL);
}

bool pf_storage_worker_submit(const char *family, const char *arguments,
                              bool host_owned, uint32_t session)
{
    storage_request_t request = { .session = session, .host_owned = host_owned };

    refresh_benchmark_completion();
    if ((requests == NULL) || (family == NULL) || (arguments == NULL) ||
        ((strcmp(family, "sd") != 0) && (strcmp(family, "fs") != 0)) ||
        (strlen(arguments) > sizeof(request.arguments) - 2u)) {
        return false;
    }
    memcpy(request.family, family, 3u);
    memcpy(request.arguments, arguments, strlen(arguments) + 1u);
    taskENTER_CRITICAL();
    if (busy || media_service_busy) {
        taskEXIT_CRITICAL();
        return false;
    }
    active_session = session;
    cancelled = false;
    busy = true;
    taskEXIT_CRITICAL();
    if (xQueueSend(requests, &request, 0u) != pdPASS) {
        taskENTER_CRITICAL();
        busy = false;
        taskEXIT_CRITICAL();
        return false;
    }
    return true;
}

bool pf_storage_worker_busy(void)
{
    refresh_benchmark_completion();
    taskENTER_CRITICAL();
    bool is_busy = busy || media_service_busy;
    taskEXIT_CRITICAL();
    return is_busy;
}

bool pf_storage_worker_recover_media(void)
{
    storage_request_t request = { .recover_media = true };
    refresh_benchmark_completion();
    taskENTER_CRITICAL();
    if (busy || media_service_busy || !config_available || requests == NULL) {
        taskEXIT_CRITICAL();
        return false;
    }
    busy = true;
    taskEXIT_CRITICAL();
    if (xQueueSend(requests, &request, 0u) != pdPASS) {
        taskENTER_CRITICAL();
        busy = false;
        taskEXIT_CRITICAL();
        return false;
    }
    return true;
}

bool pf_storage_worker_poll(pf_storage_worker_reply_t *reply)
{
    refresh_benchmark_completion();
    if ((reply == NULL) || (replies == NULL) || (xQueueReceive(replies, reply, 0u) != pdPASS)) {
        return false;
    }
    if (reply->done) {
        taskENTER_CRITICAL();
        busy = false;
        taskEXIT_CRITICAL();
    }
    return true;
}

void pf_storage_worker_cancel(uint32_t session)
{
    taskENTER_CRITICAL();
    if (busy && !benchmark_active && (active_session == session)) {
        cancelled = true;
    }
    taskEXIT_CRITICAL();
}

bool pf_storage_worker_benchmark_start(uint32_t size_mib, bool host_owned)
{
    pf_file_benchmark_error_t rejection = PF_FILE_BENCHMARK_ERROR_NONE;
    refresh_benchmark_completion();
    taskENTER_CRITICAL();
    bool unavailable = busy || media_service_busy || (requests == NULL);
    taskEXIT_CRITICAL();
    if (unavailable) {
        rejection = PF_FILE_BENCHMARK_ERROR_BUSY;
    } else if (host_owned) {
        rejection = PF_FILE_BENCHMARK_ERROR_USB_OWNER;
    } else if ((size_mib == 0u) || (size_mib > PF_FILE_BENCHMARK_MAX_MIB)) {
        rejection = PF_FILE_BENCHMARK_ERROR_SIZE;
#if PF_STORAGE_BACKEND_NOR
    } else if (!pf_nor_media_try_lock()) {
#else
    } else if (!pf_sd_try_lock()) {
#endif
        rejection = PF_FILE_BENCHMARK_ERROR_BUSY;
    } else {
#if PF_STORAGE_BACKEND_NOR
        if (!pf_nor_media_ready() || pf_nor_media_usb_owned()) {
#else
        if (!config_available || !pf_sd_is_initialized()) {
#endif
            rejection = PF_FILE_BENCHMARK_ERROR_MEDIA;
        } else if (!pf_fatfs_volume_is_mounted()) {
            rejection = PF_FILE_BENCHMARK_ERROR_NOT_MOUNTED;
        }
#if PF_STORAGE_BACKEND_NOR
        pf_nor_media_unlock();
#else
        pf_sd_unlock();
#endif
    }
    taskENTER_CRITICAL();
    if (busy || media_service_busy) {
        rejection = PF_FILE_BENCHMARK_ERROR_BUSY;
    }
    benchmark_status.request_error = rejection;
    if (rejection != PF_FILE_BENCHMARK_ERROR_NONE) {
        taskEXIT_CRITICAL();
        return false;
    }
    benchmark_status = (pf_file_benchmark_status_t){
        .state = PF_FILE_BENCHMARK_QUEUED, .target_bytes = size_mib * 1024u * 1024u
    };
    benchmark_active = true;
    benchmark_finished = benchmark_cancelled = false;
    busy = true;
    taskEXIT_CRITICAL();
    storage_request_t request = { .test_size_mib = size_mib };
    if (xQueueSend(requests, &request, 0u) != pdPASS) {
        taskENTER_CRITICAL();
        busy = benchmark_active = false;
        benchmark_status.state = PF_FILE_BENCHMARK_FAILED;
        benchmark_status.request_error = PF_FILE_BENCHMARK_ERROR_BUSY;
        taskEXIT_CRITICAL();
        return false;
    }
    return true;
}

bool pf_storage_worker_benchmark_cancel(void)
{
    refresh_benchmark_completion();
    taskENTER_CRITICAL();
    bool accepted = benchmark_active && (benchmark_status.state < PF_FILE_BENCHMARK_CLEANUP);
    if (accepted) {
        benchmark_cancelled = true;
        benchmark_status.request_error = PF_FILE_BENCHMARK_ERROR_NONE;
    } else {
        benchmark_status.request_error = PF_FILE_BENCHMARK_ERROR_BUSY;
    }
    taskEXIT_CRITICAL();
    return accepted;
}

void pf_storage_worker_benchmark_status(pf_file_benchmark_status_t *status)
{
    if (status == NULL) {
        return;
    }
    refresh_benchmark_completion();
    taskENTER_CRITICAL();
    *status = benchmark_status;
    taskEXIT_CRITICAL();
}

bool pf_storage_worker_media_absent(void)
{
#if PF_STORAGE_BACKEND_NOR
    return false;
#else
    taskENTER_CRITICAL();
    bool absent = !media_service_busy && stable_presence_valid && !stable_card_present;
    taskEXIT_CRITICAL();
    return absent;
#endif
}
