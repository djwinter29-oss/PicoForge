#include "storage_worker.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "drivers/sd_card/sd_card.h"
#include "drivers/system.h"
#include "middleware/storage/storage.h"

#include <string.h>

#define STORAGE_WORKER_STACK_SIZE 2048u
#define STORAGE_REPLY_COUNT 8u
#define CARD_PRESENCE_POLL_MS 100u
#define CARD_PRESENCE_DEBOUNCE_SAMPLES 3u
#define CARD_MOUNT_RETRY_MS 5000u

typedef struct {
    uint32_t session;
    uint32_t test_size_mib;
    bool host_owned;
    char family[3];
    char arguments[PF_STORAGE_COMMAND_ARGS_SIZE];
} storage_request_t;

static StaticTask_t worker_control;
static StackType_t worker_stack[STORAGE_WORKER_STACK_SIZE];
static StaticQueue_t request_control;
static StaticQueue_t reply_control;
static uint8_t request_storage[sizeof(storage_request_t)];
static uint8_t reply_storage[STORAGE_REPLY_COUNT * sizeof(pf_storage_worker_reply_t)];
static QueueHandle_t requests;
static QueueHandle_t replies;
static pf_sd_card_config_t card_config;
static bool config_available;
static bool busy;
static uint32_t active_session;
static volatile bool cancelled;
static pf_self_test_status_t self_test_status;
static bool self_test_active;
static bool self_test_finished;
static bool self_test_cancelled;
static bool card_service_busy;
static bool presence_sample_valid;
static bool presence_sample;
static uint8_t presence_sample_count;
static bool stable_presence_valid;
static bool stable_card_present;
static bool auto_mount_pending = true;
static bool mount_retry_waiting;
static uint32_t last_mount_attempt_ms;

static void refresh_self_test_completion(void)
{
    taskENTER_CRITICAL();
    if (self_test_active && self_test_finished) {
        self_test_active = false;
        self_test_finished = false;
        busy = false;
    }
    taskEXIT_CRITICAL();
}

static uint32_t self_test_clock(void *context)
{
    (void)context;
    return pf_system_uptime_ms();
}

static bool self_test_update(const pf_self_test_status_t *status, void *context)
{
    (void)context;
    taskENTER_CRITICAL();
    pf_self_test_error_t request_error = self_test_status.request_error;
    self_test_status = *status;
    self_test_status.request_error = request_error;
    bool keep_running = !self_test_cancelled;
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

static void attempt_auto_mount(void)
{
    pf_storage_result_t result = pf_storage_mount(&card_config, NULL);
    auto_mount_pending = (result != PF_STORAGE_OK) && (result != PF_STORAGE_ALREADY_MOUNTED);
    last_mount_attempt_ms = pf_system_uptime_ms();
    mount_retry_waiting = auto_mount_pending;
}

static void service_card_presence(void)
{
    pf_sd_card_info_t info;

    if (!config_available) {
        return;
    }
    taskENTER_CRITICAL();
    if (busy || card_service_busy) {
        taskEXIT_CRITICAL();
        return;
    }
    card_service_busy = true;
    taskEXIT_CRITICAL();

    pf_sd_card_lock();
    pf_sd_card_get_info(&info);
    bool firmware_owned = pf_sd_card_get_owner() == PF_SD_CARD_OWNER_FIRMWARE;
    bool mounted_card_uninitialized = pf_storage_is_mounted() && !info.initialized;
    if (firmware_owned && mounted_card_uninitialized && (!info.presence_known || info.present)) {
        if (pf_storage_unmount(NULL) != PF_STORAGE_OK) {
            goto done;
        }
        pf_storage_forget_filesystem_format();
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
                pf_sd_card_confirm_removed();
                if (pf_storage_is_mounted()) {
                    (void)pf_storage_unmount(NULL);
                }
                pf_storage_forget_filesystem_format();
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
    if (pf_sd_card_take_reinit_request()) {
        auto_mount_pending = true;
        mount_retry_waiting = false;
    }
    if (pf_storage_is_mounted()) {
        auto_mount_pending = false;
        goto done;
    }
    if (!auto_mount_pending) {
        goto done;
    }
    if (mount_retry_waiting &&
        ((uint32_t)(pf_system_uptime_ms() - last_mount_attempt_ms) < CARD_MOUNT_RETRY_MS)) {
        goto done;
    }
    attempt_auto_mount();

done:
    pf_sd_card_unlock();
    taskENTER_CRITICAL();
    card_service_busy = false;
    taskEXIT_CRITICAL();
}

static void storage_task(void *context)
{
    (void)context;
    storage_request_t request;

    for (;;) {
        if (xQueueReceive(requests, &request,
                          pdMS_TO_TICKS(CARD_PRESENCE_POLL_MS)) != pdPASS) {
            service_card_presence();
            continue;
        }
        if (request.test_size_mib != 0u) {
            pf_storage_self_test_run(request.test_size_mib,
                                      self_test_clock, self_test_update, NULL);
            taskENTER_CRITICAL();
            self_test_finished = true;
            taskEXIT_CRITICAL();
            continue;
        }
        bool previously_mounted = pf_storage_is_mounted();
        pf_storage_command_execute(config_available ? &card_config : NULL,
                                    request.host_owned, request.family, request.arguments,
                                    send_reply, &request);
        if (previously_mounted && !pf_storage_is_mounted()) {
            auto_mount_pending = false;
        }
        pf_storage_worker_reply_t reply = { .session = request.session, .done = true };
        (void)xQueueSend(replies, &reply, portMAX_DELAY);
    }
}

bool pf_storage_worker_init(const pf_sd_card_config_t *config)
{
    if (requests != NULL) {
        return false;
    }
    config_available = config != NULL;
    if (config_available) {
        card_config = *config;
    }
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

    refresh_self_test_completion();
    if ((requests == NULL) || (family == NULL) || (arguments == NULL) ||
        ((strcmp(family, "sd") != 0) && (strcmp(family, "fs") != 0)) ||
        (strlen(arguments) > sizeof(request.arguments) - 2u)) {
        return false;
    }
    memcpy(request.family, family, 3u);
    memcpy(request.arguments, arguments, strlen(arguments) + 1u);
    taskENTER_CRITICAL();
    if (busy || card_service_busy) {
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
    refresh_self_test_completion();
    taskENTER_CRITICAL();
    bool is_busy = busy || card_service_busy;
    taskEXIT_CRITICAL();
    return is_busy;
}

bool pf_storage_worker_poll(pf_storage_worker_reply_t *reply)
{
    refresh_self_test_completion();
    if ((reply == NULL) || (replies == NULL) || (xQueueReceive(replies, reply, 0u) != pdPASS)) {
        return false;
    }
    if (reply->done) {
        busy = false;
    }
    return true;
}

void pf_storage_worker_cancel(uint32_t session)
{
    if (busy && !self_test_active && (active_session == session)) {
        cancelled = true;
    }
}

bool pf_storage_worker_self_test_start(uint32_t size_mib, bool host_owned)
{
    pf_self_test_error_t rejection = PF_SELF_TEST_ERROR_NONE;
    refresh_self_test_completion();
    if (busy || card_service_busy || (requests == NULL)) {
        rejection = PF_SELF_TEST_ERROR_BUSY;
    } else if (host_owned) {
        rejection = PF_SELF_TEST_ERROR_USB_OWNER;
    } else if ((size_mib == 0u) || (size_mib > PF_STORAGE_SELF_TEST_MAX_MIB)) {
        rejection = PF_SELF_TEST_ERROR_SIZE;
    } else if (!config_available || !pf_sd_card_is_initialized()) {
        rejection = PF_SELF_TEST_ERROR_CARD;
    } else if (!pf_storage_is_mounted()) {
        rejection = PF_SELF_TEST_ERROR_NOT_MOUNTED;
    }
    taskENTER_CRITICAL();
    if (busy || card_service_busy) {
        rejection = PF_SELF_TEST_ERROR_BUSY;
    }
    self_test_status.request_error = rejection;
    if (rejection != PF_SELF_TEST_ERROR_NONE) {
        taskEXIT_CRITICAL();
        return false;
    }
    self_test_status = (pf_self_test_status_t){
        .state = PF_SELF_TEST_QUEUED, .target_bytes = size_mib * 1024u * 1024u
    };
    self_test_active = true;
    self_test_finished = self_test_cancelled = false;
    busy = true;
    taskEXIT_CRITICAL();
    storage_request_t request = { .test_size_mib = size_mib };
    if (xQueueSend(requests, &request, 0u) != pdPASS) {
        taskENTER_CRITICAL();
        busy = self_test_active = false;
        self_test_status.state = PF_SELF_TEST_FAILED;
        self_test_status.request_error = PF_SELF_TEST_ERROR_BUSY;
        taskEXIT_CRITICAL();
        return false;
    }
    return true;
}

bool pf_storage_worker_self_test_cancel(void)
{
    refresh_self_test_completion();
    taskENTER_CRITICAL();
    bool accepted = self_test_active && (self_test_status.state < PF_SELF_TEST_CLEANUP);
    if (accepted) {
        self_test_cancelled = true;
        self_test_status.request_error = PF_SELF_TEST_ERROR_NONE;
    } else {
        self_test_status.request_error = PF_SELF_TEST_ERROR_BUSY;
    }
    taskEXIT_CRITICAL();
    return accepted;
}

void pf_storage_worker_self_test_status(pf_self_test_status_t *status)
{
    if (status == NULL) {
        return;
    }
    refresh_self_test_completion();
    taskENTER_CRITICAL();
    *status = self_test_status;
    taskEXIT_CRITICAL();
}

bool pf_storage_worker_media_absent(void)
{
    taskENTER_CRITICAL();
    bool absent = !card_service_busy && stable_presence_valid && !stable_card_present;
    taskEXIT_CRITICAL();
    return absent;
}
