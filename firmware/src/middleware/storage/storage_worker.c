#include "storage_worker.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "drivers/sd_card/sd_card.h"

#include <string.h>

#define STORAGE_WORKER_STACK_SIZE 2048u
#define STORAGE_REPLY_COUNT 8u

typedef struct {
    uint32_t session;
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

static void storage_task(void *context)
{
    (void)context;
    storage_request_t request;

    for (;;) {
        if (xQueueReceive(requests, &request, portMAX_DELAY) != pdPASS) {
            continue;
        }
        pf_storage_command_execute(config_available ? &card_config : NULL,
                                    request.host_owned, request.family, request.arguments,
                                    send_reply, &request);
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

    if (busy || (requests == NULL) || (family == NULL) || (arguments == NULL) ||
        ((strcmp(family, "sd") != 0) && (strcmp(family, "fs") != 0)) ||
        (strlen(arguments) > sizeof(request.arguments) - 2u)) {
        return false;
    }
    memcpy(request.family, family, 3u);
    memcpy(request.arguments, arguments, strlen(arguments) + 1u);
    active_session = session;
    cancelled = false;
    busy = true;
    if (xQueueSend(requests, &request, 0u) != pdPASS) {
        busy = false;
        return false;
    }
    return true;
}

bool pf_storage_worker_busy(void)
{
    return busy;
}

bool pf_storage_worker_poll(pf_storage_worker_reply_t *reply)
{
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
    if (busy && (active_session == session)) {
        cancelled = true;
    }
}