/** @file Application storage-owner task, command sessions, and media policy. */
#include "storage_worker.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "drivers/system.h"
#include "middleware/storage/fatfs_volume.h"
#include "middleware/storage/storage.h"
#include "middleware/logging/logging.h"

#include <string.h>
#include <stdatomic.h>

#define STORAGE_WORKER_STACK_SIZE 2048u
#define STORAGE_REPLY_COUNT 8u
#define MEDIA_SERVICE_POLL_MS 100u

/** One admitted operation; only one command, benchmark or recovery may be outstanding. */
typedef struct {
    uint32_t session; /**< Originating command session. */
    uint32_t test_size_mib; /**< Nonzero selects a file benchmark. */
    bool host_owned; /**< Ownership snapshot protected by the admission gate. */
    bool recover_media; /**< Selects explicit recovery instead of a command. */
    bool return_from_usb; /**< Attempt one mount after a confirmed forced USB detach. */
    bool unmount_for_usb; /**< Unregister FatFs for the pending USB ownership transition. */
    char family[8]; /**< Command family: storage or fs. */
    char arguments[PF_STORAGE_WORKER_ARGS_SIZE]; /**< Copied command arguments. */
} storage_request_t;

static StaticTask_t worker_control;
static StackType_t worker_stack[STORAGE_WORKER_STACK_SIZE];
static StaticQueue_t request_control;
static StaticQueue_t reply_control;
static uint8_t request_storage[sizeof(storage_request_t)];
static uint8_t reply_storage[STORAGE_REPLY_COUNT * sizeof(pf_storage_worker_reply_t)];
static QueueHandle_t requests;
static QueueHandle_t replies;
static pf_storage_worker_execute_fn_t execute; /**< Command executor registered at init. */
static bool config_available;
static bool busy;
static uint32_t active_session;
/* Request admission/completion and self-test publication use critical sections.
 * The worker reads cancellation while waiting for replies; atomic publication
 * avoids relying on volatile for cross-task communication. Presence/automount
 * policy lives in the storage service; media_service_busy excludes admission while it runs. */
static atomic_bool cancelled;
#if PF_ENABLE_STORAGE_BENCHMARK
static pf_file_benchmark_status_t benchmark_status;
static bool benchmark_active;
static bool benchmark_finished;
static bool benchmark_cancelled;
static bool benchmark_admission_pending;
static bool benchmark_admission_result_ready;
static bool benchmark_admission_accepted;
static pf_file_benchmark_error_t benchmark_admission_error;
#endif
static bool media_service_busy;

static void refresh_benchmark_completion(void)
{
#if PF_ENABLE_STORAGE_BENCHMARK
    taskENTER_CRITICAL();
    if (benchmark_active && benchmark_finished) {
        benchmark_active = false;
        benchmark_finished = false;
        busy = false;
    }
    taskEXIT_CRITICAL();
#endif
}

#if PF_ENABLE_STORAGE_BENCHMARK
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
#endif

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

/** @brief Run storage policy while excluding worker admission and MSC transitions. */
static void service_media(void)
{
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
    pf_storage_poll();
    taskENTER_CRITICAL();
    media_service_busy = false;
    taskEXIT_CRITICAL();
}

/** @brief Release admission after a queued media transition. */
static void finish_media_request(void)
{
    taskENTER_CRITICAL();
    busy = false;
    taskEXIT_CRITICAL();
}

/** @brief Recover only after USB handoff; leave automatic mounting blocked for explicit inspection. */
static void recover_media(void)
{
    (void)pf_storage_recover();
    finish_media_request();
}

/** @brief Hold admission through the single forced-return mount attempt. */
static void return_from_usb(void)
{
    (void)pf_storage_return_from_usb();
    finish_media_request();
}

/** @brief Execute a command and publish completion before the USB task releases admission. */
static void execute_command(storage_request_t *request)
{
    bool previously_mounted = pf_fatfs_volume_is_mounted();
    execute(request->host_owned, request->family, request->arguments, send_reply, request);
    pf_storage_note_command(previously_mounted);
    pf_storage_worker_reply_t reply = { .session = request->session, .done = true };
    (void)xQueueSend(replies, &reply, portMAX_DELAY);
}

/** @brief Finish USB handoff preparation on the sole FatFs executor before publishing idle. */
static void unmount_for_usb(void)
{
    (void)pf_storage_unmount_for_usb();
    finish_media_request();
}

static void storage_task(void *context)
{
    (void)context;
    storage_request_t request;

    TickType_t last_stack_sample = xTaskGetTickCount();

    for (;;) {
        if ((TickType_t)(xTaskGetTickCount() - last_stack_sample) >= pdMS_TO_TICKS(30000u)) {
            PF_LOG_INFO("stack", "Storage minimum free stack: %u words",
                        (unsigned)uxTaskGetStackHighWaterMark(NULL));
            last_stack_sample = xTaskGetTickCount();
        }
        if (xQueueReceive(requests, &request,
                          pdMS_TO_TICKS(MEDIA_SERVICE_POLL_MS)) != pdPASS) {
            service_media();
            continue;
        }
        if (request.unmount_for_usb) {
            unmount_for_usb();
            continue;
        }
#if PF_ENABLE_STORAGE_BENCHMARK
        if (request.test_size_mib != 0u) {
            pf_file_benchmark_error_t preflight_error =
                pf_file_benchmark_preflight(request.test_size_mib, NULL);
            taskENTER_CRITICAL();
            benchmark_admission_pending = false;
            benchmark_admission_result_ready = true;
            benchmark_admission_accepted = preflight_error == PF_FILE_BENCHMARK_ERROR_NONE;
            benchmark_admission_error = preflight_error;
            if (benchmark_admission_accepted) {
                benchmark_status = (pf_file_benchmark_status_t){
                    .state = PF_FILE_BENCHMARK_QUEUED,
                    .target_bytes = request.test_size_mib * 1024u * 1024u,
                };
            } else {
                benchmark_status.request_error = preflight_error;
                benchmark_finished = true;
            }
            taskEXIT_CRITICAL();
            if (!benchmark_admission_accepted) {
                continue;
            }
            pf_file_benchmark_run_after_preflight(request.test_size_mib,
                                                  benchmark_clock, benchmark_update, NULL);
            taskENTER_CRITICAL();
            benchmark_finished = true;
            taskEXIT_CRITICAL();
            continue;
        }
#endif
        if (request.recover_media) {
            if (request.return_from_usb) {
                return_from_usb();
            } else {
                recover_media();
            }
            continue;
        }
        execute_command(&request);
    }
}

bool pf_storage_worker_init(pf_storage_worker_execute_fn_t executor)
{
    if ((requests != NULL) || (executor == NULL)) {
        return false;
    }
    execute = executor;
    pf_block_result_t setup_result = pf_storage_setup();
    if (setup_result != PF_BLOCK_OK && setup_result != PF_BLOCK_NOT_READY) {
        return false;
    }
    config_available = setup_result == PF_BLOCK_OK;
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
        (strlen(family) >= sizeof(request.family)) ||
        (strlen(arguments) > sizeof(request.arguments) - 2u)) {
        return false;
    }
    memcpy(request.family, family, strlen(family) + 1u);
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

/** @brief Reserve sole-worker admission before publishing either USB return policy. */
static bool queue_media_recovery(bool mount)
{
    storage_request_t request = { .recover_media = true, .return_from_usb = mount };
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

bool pf_storage_worker_recover_media(void)
{
    return queue_media_recovery(false);
}

bool pf_storage_worker_return_from_usb(void)
{
    return queue_media_recovery(true);
}

bool pf_storage_worker_unmount_for_usb(void)
{
    storage_request_t request = { .unmount_for_usb = true };
    refresh_benchmark_completion();
    taskENTER_CRITICAL();
    if (busy || media_service_busy || requests == NULL) {
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
    if (busy && (active_session == session)
#if PF_ENABLE_STORAGE_BENCHMARK
        && !benchmark_active
#endif
    ) {
        cancelled = true;
    }
    taskEXIT_CRITICAL();
}

#if PF_ENABLE_STORAGE_BENCHMARK
bool pf_storage_worker_benchmark_start(uint32_t size_mib, bool host_owned)
{
    pf_file_benchmark_error_t rejection = PF_FILE_BENCHMARK_ERROR_NONE;
    refresh_benchmark_completion();
    if ((size_mib == 0u) || (size_mib > PF_FILE_BENCHMARK_MAX_MIB)) {
        rejection = PF_FILE_BENCHMARK_ERROR_SIZE;
    } else {
        taskENTER_CRITICAL();
        bool unavailable = busy || media_service_busy || (requests == NULL);
        taskEXIT_CRITICAL();
        if (unavailable) {
            rejection = PF_FILE_BENCHMARK_ERROR_BUSY;
        } else if (host_owned) {
            rejection = PF_FILE_BENCHMARK_ERROR_USB_OWNER;
        } else {
            pf_storage_status_t storage;
            pf_storage_get_status(&storage);
            switch (storage.state) {
            case PF_STORAGE_STATE_MOUNTED: break;
            case PF_STORAGE_STATE_BUSY: rejection = PF_FILE_BENCHMARK_ERROR_BUSY; break;
            case PF_STORAGE_STATE_READY: rejection = PF_FILE_BENCHMARK_ERROR_NOT_MOUNTED; break;
            default: rejection = PF_FILE_BENCHMARK_ERROR_MEDIA; break;
            }
        }
    }
    taskENTER_CRITICAL();
    if (busy || media_service_busy) {
        rejection = PF_FILE_BENCHMARK_ERROR_BUSY;
    }
    if (rejection != PF_FILE_BENCHMARK_ERROR_NONE) {
        benchmark_status.request_error = rejection;
        taskEXIT_CRITICAL();
        return false;
    }
    benchmark_active = true;
    benchmark_finished = benchmark_cancelled = false;
    benchmark_admission_pending = true;
    benchmark_admission_result_ready = false;
    busy = true;
    taskEXIT_CRITICAL();
    storage_request_t request = { .test_size_mib = size_mib };
    if (xQueueSend(requests, &request, 0u) != pdPASS) {
        taskENTER_CRITICAL();
        busy = benchmark_active = false;
        benchmark_admission_pending = false;
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
    bool accepted = benchmark_active && !benchmark_admission_pending &&
                    (benchmark_status.state < PF_FILE_BENCHMARK_CLEANUP);
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

bool pf_storage_worker_benchmark_admission_result(bool *accepted, pf_file_benchmark_error_t *error)
{
    if ((accepted == NULL) || (error == NULL)) {
        return false;
    }
    refresh_benchmark_completion();
    taskENTER_CRITICAL();
    bool ready = benchmark_admission_result_ready;
    if (ready) {
        *accepted = benchmark_admission_accepted;
        *error = benchmark_admission_error;
        benchmark_admission_result_ready = false;
    }
    taskEXIT_CRITICAL();
    return ready;
}

#endif

bool pf_storage_worker_media_absent(void)
{
    taskENTER_CRITICAL();
    bool service_running = media_service_busy;
    taskEXIT_CRITICAL();
    return !service_running && pf_storage_media_absent();
}
