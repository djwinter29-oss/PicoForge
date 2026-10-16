#include "unity.h"
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <unistd.h>
#include "selected_transport.h"

#define FACADE_SOURCE "../../../src/drivers/sd_card/sd_card.c"
#define PORT_SOURCE "../../../src/drivers/sd_card/ports/freertos/sd_card_port.c"
#include FACADE_SOURCE
#include PORT_SOURCE

pthread_mutex_t creation_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool entered, release_io, contender_started, contender_done;
static bool initialized, contract_test;
static unsigned backend_calls, readiness_calls, expected_operation;
static pf_sd_card_result_t backend_result;
static uint32_t expected_block, expected_count;
static const void *expected_buffer;
static unsigned operation, contender;

pf_sd_card_result_t backend_init(const backend_config_t *config)
{ (void)config; initialized = true; return PF_SD_CARD_OK; }
backend_stage_t backend_stage(void) { return (backend_stage_t)99; }
bool backend_ready(void) { ++readiness_calls; return initialized; }
uint32_t backend_blocks(void) { return 100u; }
static pf_sd_card_result_t backend_access(unsigned op, uint32_t block, void *buffer, uint32_t count)
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
    return PF_SD_CARD_OK;
}
pf_sd_card_result_t backend_read(uint32_t block, void *buffer, uint32_t count)
{ return backend_access(0, block, buffer, count); }
pf_sd_card_result_t backend_write(uint32_t block, const void *buffer, uint32_t count)
{ return backend_access(1, block, (void *)buffer, count); }
pf_sd_card_result_t backend_sync(void) { return backend_access(2, 0, NULL, 0); }
void backend_info(pf_sd_card_info_t *info) { info->initialized = initialized; }
#if PF_CONFIG_SD_CARD_TRANSPORT != PF_CONFIG_SD_CARD_TRANSPORT_SPI
void pf_sdio_pio_confirm_card_removed(void) { initialized = false; }
#endif
void pf_sd_card_get_profile(pf_sd_card_profile_t *profile)
{ *profile = (pf_sd_card_profile_t){0}; }

static pf_sd_card_result_t set_owner(pf_sd_card_owner_t owner)
{
    return owner == PF_SD_CARD_OWNER_USB_MSC
        ? pf_sd_card_msc_acquire() : pf_sd_card_msc_release();
}
static pf_sd_card_result_t call_operation(unsigned op, bool msc)
{
    expected_operation = op;
    if (op == 0) return msc ? pf_sd_card_msc_read_blocks(expected_block,
        (void *)expected_buffer, expected_count) : pf_sd_card_read_blocks(expected_block,
        (void *)expected_buffer, expected_count);
    if (op == 1) return msc ? pf_sd_card_msc_write_blocks(expected_block,
        expected_buffer, expected_count) : pf_sd_card_write_blocks(expected_block,
        expected_buffer, expected_count);
    return msc ? pf_sd_card_msc_sync() : pf_sd_card_sync();
}
static void check_contracts(void)
{
    contract_test = true;
    TEST_ASSERT_TRUE(pf_sd_card_try_lock());
    TEST_ASSERT_TRUE(pf_sd_card_try_lock());
    pf_sd_card_unlock();
    pf_sd_card_unlock();
    for (unsigned op = 0; op < 3; ++op) {
        TEST_ASSERT_EQUAL(PF_SD_CARD_NOT_READY, call_operation(op, true));
        TEST_ASSERT_EQUAL(PF_SD_CARD_NOT_CONFIGURED, call_operation(op, false));
    }
    TEST_ASSERT_EQUAL_UINT(0, backend_calls);
    TEST_ASSERT_EQUAL(PF_SD_CARD_INVALID_ARGUMENT, pf_sd_card_init(NULL));
    pf_sd_card_config_t config = { .transport = OTHER_TRANSPORT };
    TEST_ASSERT_EQUAL(PF_SD_CARD_INVALID_ARGUMENT, pf_sd_card_init(&config));
    config.transport = TEST_TRANSPORT;
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sd_card_init(&config));
    initialized = false;
    TEST_ASSERT_EQUAL(PF_SD_CARD_NOT_READY, pf_sd_card_msc_acquire());
    pf_sd_card_diagnostics_t diagnostics;
    pf_sd_card_get_diagnostics(&diagnostics);
    TEST_ASSERT_NOT_EQUAL(0, diagnostics.sequence);
    TEST_ASSERT_EQUAL(PF_SD_CARD_OPERATION_MSC_ACQUIRE, diagnostics.operation);
    TEST_ASSERT_EQUAL(PF_SD_CARD_NOT_READY, diagnostics.result);
    TEST_ASSERT_EQUAL_UINT32(99, diagnostics.transport_init_stage);
    for (unsigned owner = 0; owner < 2; ++owner) {
        initialized = true;
        TEST_ASSERT_EQUAL(PF_SD_CARD_OK, set_owner((pf_sd_card_owner_t)owner));
        for (unsigned op = 0; op < 3; ++op) {
            for (unsigned result = PF_SD_CARD_OK; result <= PF_SD_CARD_IO_ERROR; ++result) {
                initialized = result != PF_SD_CARD_NOT_READY;
                backend_result = (pf_sd_card_result_t)result;
                expected_block = op == 2 ? 0 : UINT32_MAX;
                expected_count = 0;
                expected_buffer = NULL;
                unsigned before = backend_calls, ready_before = readiness_calls;
                TEST_ASSERT_EQUAL(backend_result, call_operation(op, owner != 0));
                TEST_ASSERT_EQUAL_UINT(before + 1, backend_calls);
                TEST_ASSERT_EQUAL_UINT(ready_before, readiness_calls);
                TEST_ASSERT_EQUAL(PF_SD_CARD_NOT_READY, call_operation(op, owner == 0));
                TEST_ASSERT_EQUAL_UINT(before + 1, backend_calls);
            }
        }
    }
    uint8_t data[1024] = {0};
    backend_result = PF_SD_CARD_OK;
    for (unsigned owner = 0; owner < 2; ++owner) {
        initialized = true;
        TEST_ASSERT_EQUAL(PF_SD_CARD_OK, set_owner((pf_sd_card_owner_t)owner));
        expected_block = 7;
        expected_count = 2;
        expected_buffer = data;
        for (unsigned op = 0; op < 2; ++op) {
            unsigned before = backend_calls;
            TEST_ASSERT_EQUAL(PF_SD_CARD_OK, call_operation(op, owner != 0));
            TEST_ASSERT_EQUAL_UINT(before + 1, backend_calls);
        }
    }
    TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sd_card_msc_release());
    pf_sd_card_clear_diagnostics();
    pf_sd_card_get_diagnostics(&diagnostics);
    TEST_ASSERT_NOT_EQUAL(0, diagnostics.sequence);
    TEST_ASSERT_EQUAL(PF_SD_CARD_OPERATION_NONE, diagnostics.operation);
    contract_test = false;
}

/* Thread assertions deliberately use assert: Unity's longjmp belongs to the runner thread. */
static void *io(void *unused)
{
    (void)unused;
    uint8_t data[512] = {0};
    pf_sd_card_result_t result = operation == 0 ? pf_sd_card_msc_read_blocks(0, data, 1) :
        operation == 1 ? pf_sd_card_msc_write_blocks(0, data, 1) : pf_sd_card_msc_sync();
    assert(result == PF_SD_CARD_OK);
    return NULL;
}
static void *other(void *unused)
{
    (void)unused;
    atomic_store(&contender_started, true);
    assert(!pf_sd_card_try_lock());
    if (contender == 0) pf_sd_card_confirm_removed();
    else if (contender == 1) { pf_sd_card_info_t info; pf_sd_card_get_info(&info); }
    else assert(pf_sd_card_msc_release() == PF_SD_CARD_OK);
    atomic_store(&contender_done, true);
    return NULL;
}
static void check_serialization(void)
{
    for (operation = 0; operation < 3; ++operation) {
        for (contender = 0; contender < 3; ++contender) {
            pf_sd_card_config_t config = { .transport = TEST_TRANSPORT };
            TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sd_card_msc_release());
            TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sd_card_init(&config));
            TEST_ASSERT_EQUAL(PF_SD_CARD_OK, pf_sd_card_msc_acquire());
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
