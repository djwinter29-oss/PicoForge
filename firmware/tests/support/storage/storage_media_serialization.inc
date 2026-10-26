#include "unity.h"
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <unistd.h>
#include "selected_transport.h"

#define DRIVER_SOURCE "drivers/sdcard/sd.c"
#define PORT_SOURCE "drivers/sdcard/sd_port_freertos.c"
#include DRIVER_SOURCE
#define MEDIA_SOURCE "platform/storage/storage.c"
#include MEDIA_SOURCE
#include PORT_SOURCE
bool pf_fatfs_volume_is_mounted(void) { return false; }
static pf_sd_result_t bind_and_initialize(const pf_sd_config_t *config)
{
    configured = true;
    media_config = *config;
    return initialize_media();
}
pf_sd_link_mode_t pf_sd_board_pins(uint8_t bus_gpios[6], uint8_t *detect_gpio)
{ (void)bus_gpios; (void)detect_gpio; return PF_SD_LINK_NATIVE_SD; }

pthread_mutex_t creation_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool entered, release_io, contender_started, contender_done;
static bool initialized, contract_test;
static unsigned backend_calls, readiness_calls, expected_operation;
static pf_sd_result_t backend_result;
static uint32_t expected_block, expected_count;
static const void *expected_buffer;
static unsigned operation, contender;

pf_sd_result_t backend_init(const backend_config_t *config)
{ (void)config; initialized = true; return PF_SD_OK; }
bool backend_ready(void) { ++readiness_calls; return initialized; }
uint32_t backend_blocks(void) { return 100u; }
static pf_sd_result_t backend_access(unsigned op, uint32_t block, void *buffer, uint32_t count)
{
    if (contract_test) {
        ++backend_calls;
        TEST_ASSERT_EQUAL_UINT(expected_operation, op);
        TEST_ASSERT_EQUAL_UINT32(expected_block, block);
        TEST_ASSERT_EQUAL_PTR(expected_buffer, buffer);
        TEST_ASSERT_EQUAL_UINT32(expected_count, count);
        return backend_result;
    }
    atomic_store(&entered, true);
    while (!atomic_load(&release_io)) sched_yield();
    assert(!atomic_load(&contender_done));
    return PF_SD_OK;
}
pf_sd_result_t backend_read(uint32_t block, void *buffer, uint32_t count)
{ return backend_access(0, block, buffer, count); }
pf_sd_result_t backend_write(uint32_t block, const void *buffer, uint32_t count)
{ return backend_access(1, block, (void *)buffer, count); }
pf_sd_result_t backend_sync(void) { return backend_access(2, 0, NULL, 0); }
void backend_info(pf_sd_info_t *info) { info->initialized = initialized; }
void pf_sd_backend_confirm_removed(void) { initialized = false; }

static pf_block_result_t set_owner(bool usb)
{
    return usb ? pf_storage_msc_acquire() : pf_storage_msc_release();
}
static pf_sd_result_t call_operation(unsigned op, bool msc)
{
    expected_operation = op;
    if (op == 0) return msc ? read_blocks_for_owner(true, expected_block,
        (void *)expected_buffer, expected_count) : read_blocks_for_owner(false, expected_block,
        (void *)expected_buffer, expected_count);
    if (op == 1) return msc ? write_blocks_for_owner(true, expected_block,
        expected_buffer, expected_count) : write_blocks_for_owner(false, expected_block,
        expected_buffer, expected_count);
    return msc ? sync_for_owner(true) : sync_for_owner(false);
}
static void check_contracts(void)
{
    contract_test = true;
    TEST_ASSERT_TRUE(pf_sd_try_lock());
    TEST_ASSERT_TRUE(pf_sd_try_lock());
    pf_sd_unlock();
    pf_sd_unlock();
    for (unsigned op = 0; op < 3; ++op) {
        TEST_ASSERT_EQUAL(PF_SD_NOT_READY, call_operation(op, true));
        TEST_ASSERT_EQUAL(PF_SD_NOT_CONFIGURED, call_operation(op, false));
    }
    TEST_ASSERT_EQUAL_UINT(0, backend_calls);
    TEST_ASSERT_EQUAL(PF_SD_INVALID_ARGUMENT, pf_sd_init(NULL));
    pf_sd_config_t config = { .transport.mode = OTHER_TRANSPORT };
    TEST_ASSERT_EQUAL(PF_SD_INVALID_ARGUMENT, bind_and_initialize(&config));
    config.transport.mode = TEST_TRANSPORT;
    TEST_ASSERT_EQUAL(PF_SD_OK, bind_and_initialize(&config));
    initialized = false;
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, pf_storage_msc_acquire());
    TEST_ASSERT_EQUAL(PF_STORAGE_OPERATION_MSC_ACQUIRE, last_failure);
    TEST_ASSERT_EQUAL(PF_BLOCK_NOT_READY, last_failure_result);
    for (unsigned owner = 0; owner < 2; ++owner) {
        initialized = true;
        TEST_ASSERT_EQUAL(PF_BLOCK_OK, set_owner(owner != 0));
        for (unsigned op = 0; op < 3; ++op) {
            for (unsigned result = PF_SD_OK; result <= PF_SD_IO_ERROR; ++result) {
                initialized = result != PF_SD_NOT_READY;
                backend_result = (pf_sd_result_t)result;
                expected_block = op == 2 ? 0 : UINT32_MAX;
                expected_count = 0;
                expected_buffer = NULL;
                unsigned before = backend_calls, ready_before = readiness_calls;
                TEST_ASSERT_EQUAL(backend_result, call_operation(op, owner != 0));
                TEST_ASSERT_EQUAL_UINT(before + 1, backend_calls);
                TEST_ASSERT_EQUAL_UINT(ready_before, readiness_calls);
                TEST_ASSERT_EQUAL(PF_SD_NOT_READY, call_operation(op, owner == 0));
                TEST_ASSERT_EQUAL_UINT(before + 1, backend_calls);
            }
        }
    }
    uint8_t data[1024] = {0};
    backend_result = PF_SD_OK;
    for (unsigned owner = 0; owner < 2; ++owner) {
        initialized = true;
        TEST_ASSERT_EQUAL(PF_BLOCK_OK, set_owner(owner != 0));
        expected_block = 7;
        expected_count = 2;
        expected_buffer = data;
        for (unsigned op = 0; op < 2; ++op) {
            unsigned before = backend_calls;
            TEST_ASSERT_EQUAL(PF_SD_OK, call_operation(op, owner != 0));
            TEST_ASSERT_EQUAL_UINT(before + 1, backend_calls);
        }
    }
    TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_storage_msc_release());
    contract_test = false;
}

/* Thread assertions deliberately use assert: Unity's longjmp belongs to the runner thread. */
static void *io(void *unused)
{
    (void)unused;
    uint8_t data[512] = {0};
    pf_sd_result_t result = operation == 0 ? read_blocks_for_owner(true, 0, data, 1) :
        operation == 1 ? write_blocks_for_owner(true, 0, data, 1) : sync_for_owner(true);
    assert(result == PF_SD_OK);
    return NULL;
}
static void *other(void *unused)
{
    (void)unused;
    atomic_store(&contender_started, true);
    assert(!pf_sd_try_lock());
    if (contender == 0) pf_sd_confirm_removed();
    else if (contender == 1) { pf_sd_info_t info; pf_sd_get_info(&info); }
    else assert(pf_storage_msc_release() == PF_BLOCK_OK);
    atomic_store(&contender_done, true);
    return NULL;
}
static void check_serialization(void)
{
    for (operation = 0; operation < 3; ++operation) {
        for (contender = 0; contender < 3; ++contender) {
            pf_sd_config_t config = { .transport.mode = TEST_TRANSPORT };
            TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_storage_msc_release());
            TEST_ASSERT_EQUAL(PF_SD_OK, bind_and_initialize(&config));
            TEST_ASSERT_EQUAL(PF_BLOCK_OK, pf_storage_msc_acquire());
            entered = release_io = contender_started = contender_done = false;
            pthread_t a, b;
            TEST_ASSERT_EQUAL_INT(0, pthread_create(&a, NULL, io, NULL));
            while (!entered) sched_yield();
            TEST_ASSERT_EQUAL_INT(0, pthread_create(&b, NULL, other, NULL));
            while (!contender_started) sched_yield();
            for (unsigned i = 0; i < 10000; ++i) sched_yield();
            bool completed_early = contender_done;
            release_io = true;
            TEST_ASSERT_EQUAL_INT(0, pthread_join(a, NULL));
            TEST_ASSERT_EQUAL_INT(0, pthread_join(b, NULL));
            TEST_ASSERT_FALSE(completed_early);
            TEST_ASSERT_TRUE(contender_done);
        }
    }
}
