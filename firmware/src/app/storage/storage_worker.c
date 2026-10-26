/**
 * @file
 * @brief Application storage-owner task, command sessions, and media policy.
 *
 * One static task executes all FatFs work. Admission is the @c busy flag: it is taken by the USB task when a request
 * is queued and released only when the matching completion reply is polled (or, for benchmarks, when the USB task
 * observes completion). Because only the USB task submits and polls, one request is outstanding at a time.
 *
 * Locking policy: @c busy, @c media_service_busy, @c active_session and the benchmark state are shared between the
 * USB task and the worker and are only touched inside taskENTER_CRITICAL()/taskEXIT_CRITICAL() sections, which never
 * call blocking or FatFs code. @c cancelled is an atomic flag because the worker polls it while waiting on the reply
 * queue.
 */
#include "storage_worker.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "platform/system.h"
#include "platform/storage/fatfs_volume.h"
#include "platform/storage/storage.h"
#include "platform/logging.h"

#include <string.h>
#include <stdatomic.h>

/** Worker task stack depth in words; the high-water mark is logged periodically for tuning. */
#define STORAGE_WORKER_STACK_SIZE 2048u
/** Reply queue depth; a full queue applies backpressure to the executing command. */
#define STORAGE_REPLY_COUNT 8u
/** Idle wait on the request queue before the media presence/automount policy runs. */
#define MEDIA_SERVICE_POLL_MS 100u
/** Reply-queue send timeout; the loop re-checks cancellation between attempts. */
#define REPLY_RETRY_MS 50u
/** Request family field size, including the terminator. */
#define REQUEST_FAMILY_SIZE 8u

/** One admitted operation; only one command, benchmark or recovery may be outstanding. */
typedef struct {
    uint32_t session; /**< Originating command session. */
    uint32_t test_size_mib; /**< Nonzero selects a file benchmark. */
    bool host_owned; /**< Ownership snapshot protected by the admission gate. */
    bool recover_media; /**< Selects explicit recovery instead of a command. */
    bool return_from_usb; /**< Attempt one mount after a confirmed forced USB detach. */
    bool unmount_for_usb; /**< Unregister FatFs for the pending USB ownership transition. */
    char family[REQUEST_FAMILY_SIZE]; /**< Command family: storage or fs. */
    char arguments[PF_STORAGE_WORKER_ARGS_SIZE]; /**< Copied command arguments. */
} storage_request_t;

static StaticTask_t worker_control; /**< Worker task control block. */
static StackType_t worker_stack[STORAGE_WORKER_STACK_SIZE]; /**< Worker task stack. */
static StaticQueue_t request_control; /**< Request queue control block. */
static StaticQueue_t reply_control; /**< Reply queue control block. */
static uint8_t request_storage[sizeof(storage_request_t)]; /**< Single-slot request queue storage. */
static uint8_t reply_storage[STORAGE_REPLY_COUNT * sizeof(pf_storage_worker_reply_t)]; /**< Reply queue storage. */
static QueueHandle_t requests; /**< Request queue; NULL until init. */
static QueueHandle_t replies; /**< Reply queue drained by the USB task. */
static pf_storage_worker_execute_fn_t execute; /**< Command executor registered at init. */
static bool config_available; /**< Board SD configuration exists, so mounting and recovery are possible. */
static bool busy; /**< Admission flag: a request or its undrained replies are outstanding. */
static uint32_t active_session; /**< Session whose read-only output a cancel may stop. */
static atomic_bool cancelled; /**< Stop emitting replies for the active session. */
static bool media_service_busy; /**< Storage presence/automount policy is running; excludes admission. */
#if PF_ENABLE_STORAGE_BENCHMARK
/** Benchmark lifecycle and admission handshake, all guarded by the critical-section policy above. */
static struct {
    pf_file_benchmark_status_t status; /**< Latest snapshot; persists after completion. */
    bool active; /**< A benchmark owns admission, from start until the USB task observes completion. */
    bool finished; /**< The worker is done; the next USB-task call releases admission. */
    bool cancelled; /**< Cancellation requested; cleanup still runs. */
    bool admission_pending; /**< Start queued but the worker preflight has not run yet. */
    bool admission_result_ready; /**< A preflight result awaits pickup. */
    bool admission_accepted; /**< Preflight outcome. */
    pf_file_benchmark_error_t admission_error; /**< Preflight rejection reason. */
} bench;
#endif

/**
 * @brief Release admission once a finished benchmark is observed.
 * @note Called at the top of every USB-task entry point on purpose: the worker never clears @c busy for a benchmark,
 *       so the USB task sees the final status and admission change together, never a half-finished run.
 */
static void refresh_benchmark_completion(void)
{
#if PF_ENABLE_STORAGE_BENCHMARK
    taskENTER_CRITICAL();
    if (bench.active && bench.finished) {
        bench.active = false;
        bench.finished = false;
        busy = false;
    }
    taskEXIT_CRITICAL();
#endif
}

#if PF_ENABLE_STORAGE_BENCHMARK
/** @brief Worker clock handed to the benchmark. */
static uint32_t benchmark_clock(void *context)
{
    (void)context;
    return pf_system_uptime_ms();
}

/** @brief Publish a benchmark snapshot, keeping the start/cancel rejection; false once cancellation is requested. */
static bool benchmark_update(const pf_file_benchmark_status_t *status, void *context)
{
    (void)context;
    taskENTER_CRITICAL();
    pf_file_benchmark_error_t request_error = bench.status.request_error;
    bench.status = *status;
    bench.status.request_error = request_error;
    bool keep_running = !bench.cancelled;
    taskEXIT_CRITICAL();
    return keep_running;
}
#endif

/**
 * @brief Queue one output line for the USB task, waiting for queue space until the session is cancelled.
 * @param line NUL-terminated text shorter than the reply line size.
 * @param context The executing storage_request_t.
 * @return False when the line is too long or the session was cancelled.
 */
static bool send_reply(const char *line, void *context)
{
    const storage_request_t *request = context;
    pf_storage_worker_reply_t reply = { .session = request->session };
    size_t length = strlen(line);

    if (length >= sizeof(reply.line)) {
        return false;
    }
    memcpy(reply.line, line, length + 1u);
    while (!cancelled) {
        if (xQueueSend(replies, &reply, pdMS_TO_TICKS(REPLY_RETRY_MS)) == pdPASS) {
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
    /* The completion marker must always be delivered: draining it is what releases admission. The USB task keeps
     * draining replies (including those of cancelled sessions), so this wait is bounded by that drain. */
    (void)xQueueSend(replies, &reply, portMAX_DELAY);
}

/** @brief Finish USB handoff preparation on the sole FatFs executor before publishing idle. */
static void unmount_for_usb(void)
{
    (void)pf_storage_unmount_for_usb();
    finish_media_request();
}

#if PF_ENABLE_STORAGE_BENCHMARK
/** @brief Preflight, publish the admission result, then run an accepted benchmark and mark it finished. */
static void run_benchmark_request(const storage_request_t *request)
{
    pf_file_benchmark_error_t preflight_error = pf_file_benchmark_preflight(request->test_size_mib, NULL);
    bool accepted = preflight_error == PF_FILE_BENCHMARK_ERROR_NONE;

    taskENTER_CRITICAL();
    bench.admission_pending = false;
    bench.admission_result_ready = true;
    bench.admission_accepted = accepted;
    bench.admission_error = preflight_error;
    if (accepted) {
        bench.status = (pf_file_benchmark_status_t){
            .state = PF_FILE_BENCHMARK_QUEUED,
            .target_bytes = PF_FILE_BENCHMARK_BYTES(request->test_size_mib),
        };
    } else {
        bench.status.request_error = preflight_error;
        bench.finished = true;
    }
    taskEXIT_CRITICAL();
    if (!accepted) {
        return;
    }
    pf_file_benchmark_run_after_preflight(request->test_size_mib, benchmark_clock, benchmark_update, NULL);
    taskENTER_CRITICAL();
    bench.finished = true;
    taskEXIT_CRITICAL();
}
#endif

/** @brief Worker task: serve queued requests, run the media policy when idle, and log stack headroom. */
static void storage_task(void *context)
{
    (void)context;
    storage_request_t request;

    TickType_t last_stack_sample = xTaskGetTickCount();

    for (;;) {
        /* Stack high-water log period (30 s); kept literal because contract tests pin it. */
        if ((TickType_t)(xTaskGetTickCount() - last_stack_sample) >= pdMS_TO_TICKS(30000u)) {
            PF_LOGGING_INFO("stack", "Storage minimum free stack: %u words",
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
            run_benchmark_request(&request);
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

/**
 * @brief Take sole-worker admission and queue one request; release admission if the queue rejects it.
 * @param request Request copied into the queue.
 * @param need_config Require board configuration (media recovery paths).
 * @param start_session Begin a new command session: record @c request->session and clear cancellation.
 */
static bool enqueue_request(const storage_request_t *request, bool need_config, bool start_session)
{
    taskENTER_CRITICAL();
    if (busy || media_service_busy || requests == NULL || (need_config && !config_available)) {
        taskEXIT_CRITICAL();
        return false;
    }
    if (start_session) {
        active_session = request->session;
        cancelled = false;
    }
    busy = true;
    taskEXIT_CRITICAL();
    if (xQueueSend(requests, request, 0u) != pdPASS) {
        taskENTER_CRITICAL();
        busy = false;
        taskEXIT_CRITICAL();
        return false;
    }
    return true;
}

bool pf_storage_worker_submit(const char *family, const char *arguments,
                              bool host_owned, uint32_t session)
{
    storage_request_t request = { .session = session, .host_owned = host_owned };

    refresh_benchmark_completion();
    if ((requests == NULL) || (family == NULL) || (arguments == NULL)) {
        return false;
    }
    size_t family_length = strlen(family);
    size_t arguments_length = strlen(arguments);
    if ((family_length >= sizeof(request.family)) || (arguments_length > sizeof(request.arguments) - 2u)) {
        return false;
    }
    memcpy(request.family, family, family_length + 1u);
    memcpy(request.arguments, arguments, arguments_length + 1u);
    return enqueue_request(&request, false, true);
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
    return enqueue_request(&request, true, false);
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
    return enqueue_request(&request, false, false);
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
        && !bench.active
#endif
    ) {
        cancelled = true;
    }
    taskEXIT_CRITICAL();
}

#if PF_ENABLE_STORAGE_BENCHMARK
/* Benchmark admission stays separate from enqueue_request(): it must set the bench flags in the same critical section
 * that takes @c busy, and it records rejections in the benchmark status instead of only returning false. */
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
        bench.status.request_error = rejection;
        taskEXIT_CRITICAL();
        return false;
    }
    bench.active = true;
    bench.finished = bench.cancelled = false;
    bench.admission_pending = true;
    bench.admission_result_ready = false;
    busy = true;
    taskEXIT_CRITICAL();
    storage_request_t request = { .test_size_mib = size_mib };
    if (xQueueSend(requests, &request, 0u) != pdPASS) {
        taskENTER_CRITICAL();
        busy = bench.active = false;
        bench.admission_pending = false;
        bench.status.request_error = PF_FILE_BENCHMARK_ERROR_BUSY;
        taskEXIT_CRITICAL();
        return false;
    }
    return true;
}

bool pf_storage_worker_benchmark_cancel(void)
{
    refresh_benchmark_completion();
    taskENTER_CRITICAL();
    bool accepted = bench.active && !bench.admission_pending &&
                    (bench.status.state < PF_FILE_BENCHMARK_CLEANUP);
    if (accepted) {
        bench.cancelled = true;
        bench.status.request_error = PF_FILE_BENCHMARK_ERROR_NONE;
    } else {
        bench.status.request_error = PF_FILE_BENCHMARK_ERROR_BUSY;
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
    *status = bench.status;
    taskEXIT_CRITICAL();
}

bool pf_storage_worker_benchmark_admission_result(bool *accepted, pf_file_benchmark_error_t *error)
{
    if ((accepted == NULL) || (error == NULL)) {
        return false;
    }
    refresh_benchmark_completion();
    taskENTER_CRITICAL();
    bool ready = bench.admission_result_ready;
    if (ready) {
        *accepted = bench.admission_accepted;
        *error = bench.admission_error;
        bench.admission_result_ready = false;
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
