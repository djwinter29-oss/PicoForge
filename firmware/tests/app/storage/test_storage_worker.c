#include "unity.h"
#define PF_ENABLE_STORAGE_BENCHMARK 1
#include "Mocklogging.h"
#include "Mocksystem.h"
#include "Mockstorage.h"
#include "Mockfatfs_volume.h"
#include "Mockfile_benchmark.h"
TEST_INCLUDE_PATH("firmware/tests/support/storage")

#define WORKER_SOURCE "app/storage/storage_worker.c"
#include WORKER_SOURCE

#include <string.h>

#define STUB_QUEUE_DEPTH 8u

/** Minimal FIFO standing in for a FreeRTOS static queue. */
typedef struct {
    size_t size;
    unsigned count;
    unsigned char items[STUB_QUEUE_DEPTH][512];
} stub_queue_t;

static stub_queue_t request_queue, reply_queue;
static bool send_fails;
static unsigned executed;
static bool executed_host_owned;
static char executed_family[8], executed_arguments[PF_STORAGE_WORKER_ARGS_SIZE];

QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t size, uint8_t *storage, StaticQueue_t *control)
{
    stub_queue_t *queue = length == 1u ? &request_queue : &reply_queue;
    (void)storage;
    (void)control;
    queue->size = size;
    return queue;
}

BaseType_t xQueueSend(QueueHandle_t handle, const void *item, TickType_t timeout)
{
    stub_queue_t *queue = handle;
    (void)timeout;
    if (send_fails || (queue->count == STUB_QUEUE_DEPTH)) {
        return 0;
    }
    memcpy(queue->items[queue->count++], item, queue->size);
    return pdPASS;
}

BaseType_t xQueueReceive(QueueHandle_t handle, void *item, TickType_t timeout)
{
    stub_queue_t *queue = handle;
    (void)timeout;
    if (queue->count == 0u) {
        return 0;
    }
    memcpy(item, queue->items[0], queue->size);
    memmove(queue->items[0], queue->items[1], (STUB_QUEUE_DEPTH - 1u) * sizeof(queue->items[0]));
    --queue->count;
    return pdPASS;
}

TaskHandle_t xTaskCreateStatic(void (*entry)(void *), const char *name, uint32_t depth, void *argument,
                               UBaseType_t priority, StackType_t *stack, StaticTask_t *control)
{
    (void)entry; (void)name; (void)depth; (void)argument; (void)priority; (void)stack; (void)control;
    return (TaskHandle_t)1;
}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 0u; }
TickType_t xTaskGetTickCount(void) { return 0u; }

static void fake_execute(bool host_owned, const char *family, const char *arguments,
                         pf_storage_worker_reply_fn_t reply, void *context)
{
    ++executed;
    executed_host_owned = host_owned;
    strcpy(executed_family, family);
    strcpy(executed_arguments, arguments);
    TEST_ASSERT_TRUE(reply("line", context));
}

static storage_request_t queued_request(void)
{
    storage_request_t request;
    TEST_ASSERT_EQUAL_UINT(1u, request_queue.count);
    memcpy(&request, request_queue.items[0], sizeof(request));
    return request;
}

void setUp(void)
{
    memset(&request_queue, 0, sizeof(request_queue));
    memset(&reply_queue, 0, sizeof(reply_queue));
    request_queue.size = sizeof(storage_request_t);
    reply_queue.size = sizeof(pf_storage_worker_reply_t);
    requests = &request_queue;
    replies = &reply_queue;
    execute = fake_execute;
    config_available = true;
    busy = false;
    media_service_busy = false;
    active_session = 0u;
    cancelled = false;
    send_fails = false;
    executed = 0u;
    memset(&bench.status, 0, sizeof(bench.status));
    bench.active = bench.finished = bench.cancelled = false;
    bench.admission_pending = bench.admission_result_ready = false;
}
void tearDown(void) {}

void test_init_creates_queues_once_and_rejects_a_null_executor(void)
{
    requests = NULL;
    replies = NULL;
    TEST_ASSERT_FALSE(pf_storage_worker_init(NULL));
    pf_storage_setup_ExpectAndReturn(PF_BLOCK_NOT_READY);
    TEST_ASSERT_TRUE(pf_storage_worker_init(fake_execute));
    TEST_ASSERT_FALSE(config_available);
    TEST_ASSERT_FALSE(pf_storage_worker_init(fake_execute));
}

void test_init_fails_when_board_setup_fails(void)
{
    requests = NULL;
    pf_storage_setup_ExpectAndReturn(PF_BLOCK_IO_ERROR);
    TEST_ASSERT_FALSE(pf_storage_worker_init(fake_execute));
}

void test_submit_copies_strings_and_holds_admission_until_done_is_polled(void)
{
    pf_storage_worker_reply_t reply;

    TEST_ASSERT_TRUE(pf_storage_worker_submit("fs", "ls /", true, 7u));
    TEST_ASSERT_TRUE(pf_storage_worker_busy());
    TEST_ASSERT_EQUAL_STRING("fs", queued_request().family);
    TEST_ASSERT_EQUAL_STRING("ls /", queued_request().arguments);
    TEST_ASSERT_TRUE(queued_request().host_owned);
    TEST_ASSERT_EQUAL_UINT32(7u, queued_request().session);
    TEST_ASSERT_FALSE(pf_storage_worker_submit("fs", "ls /", false, 8u));

    reply_queue.count = 0u;
    pf_storage_worker_reply_t line = { .session = 7u, .line = "x" };
    TEST_ASSERT_EQUAL(pdPASS, xQueueSend(replies, &line, 0u));
    TEST_ASSERT_TRUE(pf_storage_worker_poll(&reply));
    TEST_ASSERT_TRUE(pf_storage_worker_busy());
    pf_storage_worker_reply_t done = { .session = 7u, .done = true };
    TEST_ASSERT_EQUAL(pdPASS, xQueueSend(replies, &done, 0u));
    TEST_ASSERT_TRUE(pf_storage_worker_poll(&reply));
    TEST_ASSERT_FALSE(pf_storage_worker_busy());
    TEST_ASSERT_FALSE(pf_storage_worker_poll(&reply));
}

void test_submit_releases_admission_when_the_queue_rejects(void)
{
    send_fails = true;
    TEST_ASSERT_FALSE(pf_storage_worker_submit("fs", "ls", false, 1u));
    TEST_ASSERT_FALSE(pf_storage_worker_busy());
}

void test_submit_rejects_invalid_and_oversized_input_without_taking_admission(void)
{
    char arguments[PF_STORAGE_WORKER_ARGS_SIZE];

    memset(arguments, 'a', sizeof(arguments));
    arguments[sizeof(arguments) - 1u] = '\0';
    TEST_ASSERT_FALSE(pf_storage_worker_submit(NULL, "x", false, 1u));
    TEST_ASSERT_FALSE(pf_storage_worker_submit("fs", NULL, false, 1u));
    TEST_ASSERT_FALSE(pf_storage_worker_submit("longfamily", "x", false, 1u));
    TEST_ASSERT_FALSE(pf_storage_worker_submit("fs", arguments, false, 1u));
    TEST_ASSERT_FALSE(pf_storage_worker_busy());
    requests = NULL;
    TEST_ASSERT_FALSE(pf_storage_worker_submit("fs", "x", false, 1u));
}

void test_submit_is_refused_while_the_media_service_runs(void)
{
    media_service_busy = true;
    TEST_ASSERT_FALSE(pf_storage_worker_submit("fs", "ls", false, 1u));
    TEST_ASSERT_TRUE(pf_storage_worker_busy());
}

void test_cancel_only_affects_the_active_session(void)
{
    TEST_ASSERT_TRUE(pf_storage_worker_submit("fs", "ls", false, 5u));
    pf_storage_worker_cancel(6u);
    TEST_ASSERT_FALSE(cancelled);
    pf_storage_worker_cancel(5u);
    TEST_ASSERT_TRUE(cancelled);
}

void test_cancel_does_nothing_when_idle(void)
{
    pf_storage_worker_cancel(0u);
    TEST_ASSERT_FALSE(cancelled);
}

void test_reply_is_dropped_after_cancel_and_oversized_lines_are_refused(void)
{
    storage_request_t request = { .session = 3u };
    char long_line[PF_STORAGE_WORKER_REPLY_SIZE + 1u];

    TEST_ASSERT_TRUE(send_reply("ok", &request));
    TEST_ASSERT_EQUAL_UINT(1u, reply_queue.count);
    memset(long_line, 'a', sizeof(long_line) - 1u);
    long_line[sizeof(long_line) - 1u] = '\0';
    TEST_ASSERT_FALSE(send_reply(long_line, &request));
    TEST_ASSERT_EQUAL_UINT(1u, reply_queue.count);
    cancelled = true;
    TEST_ASSERT_FALSE(send_reply("late", &request));
    TEST_ASSERT_EQUAL_UINT(1u, reply_queue.count);
}

void test_execute_command_runs_the_executor_notifies_storage_and_queues_done(void)
{
    storage_request_t request = { .session = 9u, .host_owned = true, .family = "fs", .arguments = "ls /" };
    pf_storage_worker_reply_t reply;

    pf_fatfs_volume_is_mounted_ExpectAndReturn(true);
    pf_storage_note_command_Expect(true);
    execute_command(&request);
    TEST_ASSERT_EQUAL_UINT(1u, executed);
    TEST_ASSERT_TRUE(executed_host_owned);
    TEST_ASSERT_EQUAL_STRING("fs", executed_family);
    TEST_ASSERT_EQUAL_STRING("ls /", executed_arguments);
    TEST_ASSERT_EQUAL_UINT(2u, reply_queue.count);
    TEST_ASSERT_EQUAL(pdPASS, xQueueReceive(replies, &reply, 0u));
    TEST_ASSERT_FALSE(reply.done);
    TEST_ASSERT_EQUAL_STRING("line", reply.line);
    TEST_ASSERT_EQUAL(pdPASS, xQueueReceive(replies, &reply, 0u));
    TEST_ASSERT_TRUE(reply.done);
    TEST_ASSERT_EQUAL_UINT32(9u, reply.session);
}

void test_recovery_requests_reserve_admission_and_pick_the_right_policy(void)
{
    TEST_ASSERT_TRUE(pf_storage_worker_recover_media());
    TEST_ASSERT_TRUE(queued_request().recover_media);
    TEST_ASSERT_FALSE(queued_request().return_from_usb);
    TEST_ASSERT_FALSE(pf_storage_worker_return_from_usb());
    busy = false;
    request_queue.count = 0u;
    TEST_ASSERT_TRUE(pf_storage_worker_return_from_usb());
    TEST_ASSERT_TRUE(queued_request().return_from_usb);
}

void test_recovery_needs_board_configuration_and_a_queue_slot(void)
{
    config_available = false;
    TEST_ASSERT_FALSE(pf_storage_worker_recover_media());
    config_available = true;
    send_fails = true;
    TEST_ASSERT_FALSE(pf_storage_worker_return_from_usb());
    TEST_ASSERT_FALSE(pf_storage_worker_busy());
}

void test_unmount_for_usb_requires_idle_and_releases_on_queue_failure(void)
{
    busy = true;
    TEST_ASSERT_FALSE(pf_storage_worker_unmount_for_usb());
    busy = false;
    send_fails = true;
    TEST_ASSERT_FALSE(pf_storage_worker_unmount_for_usb());
    TEST_ASSERT_FALSE(pf_storage_worker_busy());
    send_fails = false;
    TEST_ASSERT_TRUE(pf_storage_worker_unmount_for_usb());
    TEST_ASSERT_TRUE(queued_request().unmount_for_usb);
    TEST_ASSERT_TRUE(pf_storage_worker_busy());
}

void test_media_service_runs_only_when_idle_and_configured(void)
{
    busy = true;
    service_media();
    busy = false;
    config_available = false;
    service_media();
    config_available = true;
    pf_storage_poll_Expect();
    service_media();
    TEST_ASSERT_FALSE(media_service_busy);
}

void test_media_absent_is_hidden_while_the_media_service_runs(void)
{
    media_service_busy = true;
    TEST_ASSERT_FALSE(pf_storage_worker_media_absent());
    media_service_busy = false;
    pf_storage_media_absent_ExpectAndReturn(true);
    TEST_ASSERT_TRUE(pf_storage_worker_media_absent());
}

void test_benchmark_start_reports_busy_and_ownership_rejections(void)
{
    pf_file_benchmark_status_t status;

    busy = true;
    TEST_ASSERT_FALSE(pf_storage_worker_benchmark_start(1u, false));
    pf_storage_worker_benchmark_status(&status);
    TEST_ASSERT_EQUAL(PF_FILE_BENCHMARK_ERROR_BUSY, status.request_error);
    busy = false;
    TEST_ASSERT_FALSE(pf_storage_worker_benchmark_start(0u, false));
    pf_storage_worker_benchmark_status(&status);
    TEST_ASSERT_EQUAL(PF_FILE_BENCHMARK_ERROR_SIZE, status.request_error);
    TEST_ASSERT_FALSE(pf_storage_worker_benchmark_start(1u, true));
    pf_storage_worker_benchmark_status(&status);
    TEST_ASSERT_EQUAL(PF_FILE_BENCHMARK_ERROR_USB_OWNER, status.request_error);
}

void test_benchmark_start_queues_one_request_and_cancel_waits_for_admission(void)
{
    pf_storage_status_t storage = { .state = PF_STORAGE_STATE_MOUNTED };

    pf_storage_get_status_ExpectAnyArgs();
    pf_storage_get_status_ReturnThruPtr_status(&storage);
    TEST_ASSERT_TRUE(pf_storage_worker_benchmark_start(2u, false));
    TEST_ASSERT_EQUAL_UINT32(2u, queued_request().test_size_mib);
    TEST_ASSERT_TRUE(pf_storage_worker_busy());
    TEST_ASSERT_FALSE(pf_storage_worker_benchmark_cancel());
    bench.admission_pending = false;
    TEST_ASSERT_TRUE(pf_storage_worker_benchmark_cancel());
    TEST_ASSERT_TRUE(bench.cancelled);
    bench.finished = true;
    TEST_ASSERT_FALSE(pf_storage_worker_busy());
}

void test_benchmark_start_reports_media_state_rejections(void)
{
    pf_file_benchmark_status_t status;
    pf_storage_status_t storage = { .state = PF_STORAGE_STATE_READY };

    pf_storage_get_status_ExpectAnyArgs();
    pf_storage_get_status_ReturnThruPtr_status(&storage);
    TEST_ASSERT_FALSE(pf_storage_worker_benchmark_start(1u, false));
    pf_storage_worker_benchmark_status(&status);
    TEST_ASSERT_EQUAL(PF_FILE_BENCHMARK_ERROR_NOT_MOUNTED, status.request_error);
    storage.state = PF_STORAGE_STATE_ABSENT;
    pf_storage_get_status_ExpectAnyArgs();
    pf_storage_get_status_ReturnThruPtr_status(&storage);
    TEST_ASSERT_FALSE(pf_storage_worker_benchmark_start(1u, false));
    pf_storage_worker_benchmark_status(&status);
    TEST_ASSERT_EQUAL(PF_FILE_BENCHMARK_ERROR_MEDIA, status.request_error);
}

void test_cancel_ignores_a_running_benchmark(void)
{
    busy = true;
    active_session = 4u;
    bench.active = true;
    pf_storage_worker_cancel(4u);
    TEST_ASSERT_FALSE(cancelled);
}

void test_rejected_benchmark_preflight_publishes_the_error_and_finishes(void)
{
    storage_request_t request = { .test_size_mib = 3u };
    bool accepted = true;
    pf_file_benchmark_error_t error = PF_FILE_BENCHMARK_ERROR_NONE;

    bench.active = bench.admission_pending = busy = true;
    pf_file_benchmark_preflight_ExpectAndReturn(3u, NULL, PF_FILE_BENCHMARK_ERROR_SPACE);
    run_benchmark_request(&request);
    TEST_ASSERT_TRUE(pf_storage_worker_benchmark_admission_result(&accepted, &error));
    TEST_ASSERT_FALSE(accepted);
    TEST_ASSERT_EQUAL(PF_FILE_BENCHMARK_ERROR_SPACE, error);
    TEST_ASSERT_FALSE(pf_storage_worker_benchmark_admission_result(&accepted, &error));
    TEST_ASSERT_FALSE(pf_storage_worker_busy());
}

void test_accepted_benchmark_preflight_runs_and_releases_admission_on_completion(void)
{
    storage_request_t request = { .test_size_mib = 3u };
    bool accepted = false;
    pf_file_benchmark_error_t error = PF_FILE_BENCHMARK_ERROR_SPACE;
    pf_file_benchmark_status_t status;

    bench.active = bench.admission_pending = busy = true;
    pf_file_benchmark_preflight_ExpectAndReturn(3u, NULL, PF_FILE_BENCHMARK_ERROR_NONE);
    pf_file_benchmark_run_after_preflight_Expect(3u, benchmark_clock, benchmark_update, NULL);
    run_benchmark_request(&request);
    TEST_ASSERT_TRUE(pf_storage_worker_benchmark_admission_result(&accepted, &error));
    TEST_ASSERT_TRUE(accepted);
    TEST_ASSERT_EQUAL(PF_FILE_BENCHMARK_ERROR_NONE, error);
    pf_storage_worker_benchmark_status(&status);
    TEST_ASSERT_EQUAL(PF_FILE_BENCHMARK_QUEUED, status.state);
    TEST_ASSERT_EQUAL_UINT32(3u * 1024u * 1024u, status.target_bytes);
    TEST_ASSERT_FALSE(pf_storage_worker_busy());
}
