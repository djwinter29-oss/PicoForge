#!/usr/bin/env python3
"""Compile the complete production facade and force concurrent backend access."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include "sd_card.h"
#include "sd_card_msc.h"
static atomic_bool entered, release_io, contender_started, contender_done;
static bool initialized;
static bool contract_test;
static unsigned backend_calls, readiness_calls, expected_operation;
static pf_sd_card_result_t backend_result;
static uint32_t expected_block, expected_count;
static const void *expected_buffer;
pf_sd_card_result_t pf_sdio_pio_init(const pf_sdio_pio_config_t *c)
{ (void)c; initialized = true; return PF_SD_CARD_OK; }
pf_sdio_pio_init_stage_t pf_sdio_pio_get_init_stage(void)
{ return (pf_sdio_pio_init_stage_t)99; }
bool pf_sdio_pio_is_initialized(void) { ++readiness_calls; return initialized; }
uint32_t pf_sdio_pio_block_count(void) { return 100; }
static pf_sd_card_result_t backend_access(unsigned op, uint32_t b, void *p, uint32_t n)
{
    if (contract_test) {
        ++backend_calls;
        assert(op == expected_operation);
        assert(b == expected_block && p == expected_buffer && n == expected_count);
        return backend_result;
    }
    (void)b; (void)p; (void)n;
    atomic_store(&entered, true);
    while (!atomic_load(&release_io)) sched_yield();
    assert(!atomic_load(&contender_done));
    return PF_SD_CARD_OK;
}
pf_sd_card_result_t pf_sdio_pio_read_blocks(uint32_t b, void *p, uint32_t n)
{ return backend_access(0, b, p, n); }
pf_sd_card_result_t pf_sdio_pio_write_blocks(uint32_t b, const void *p, uint32_t n)
{ return backend_access(1, b, (void *)p, n); }
pf_sd_card_result_t pf_sdio_pio_sync(void)
{ return backend_access(2, 0, NULL, 0); }
void pf_sdio_pio_get_info(pf_sd_card_info_t *p) { p->initialized = initialized; }
void pf_sdio_pio_confirm_card_removed(void) { initialized = false; }
void pf_sd_card_get_profile(pf_sd_card_profile_t *p) { *p = (pf_sd_card_profile_t){0}; }
static pf_sd_card_result_t set_owner(pf_sd_card_owner_t owner)
{
    return owner == PF_SD_CARD_OWNER_USB_MSC
        ? pf_sd_card_msc_acquire() : pf_sd_card_msc_release();
}
static unsigned operation, contender;
static void *io(void *unused)
{
    (void)unused; uint8_t data[512] = {0};
    pf_sd_card_result_t r = operation == 0 ? pf_sd_card_msc_read_blocks(0, data, 1) :
        operation == 1 ? pf_sd_card_msc_write_blocks(0, data, 1) : pf_sd_card_msc_sync();
    assert(r == PF_SD_CARD_OK); return NULL;
}
static void *other(void *unused)
{
    (void)unused; atomic_store(&contender_started, true);
    if (contender == 0) pf_sd_card_confirm_removed();
    else if (contender == 1) { pf_sd_card_info_t info; pf_sd_card_get_info(&info); }
    else assert(pf_sd_card_msc_release() == PF_SD_CARD_OK);
    atomic_store(&contender_done, true); return NULL;
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
    /* Wrong owner wins over unconfigured and invalid transfer arguments. */
    for (unsigned op = 0; op < 3; ++op) {
        assert(call_operation(op, true) == PF_SD_CARD_NOT_READY);
        assert(call_operation(op, false) == PF_SD_CARD_NOT_CONFIGURED);
    }
    assert(backend_calls == 0);
    assert(pf_sd_card_init(NULL) == PF_SD_CARD_INVALID_ARGUMENT);
    pf_sd_card_config_t config = { .transport = PF_SD_CARD_TRANSPORT_SDIO_PIO };
    config.transport = PF_SD_CARD_TRANSPORT_SPI;
    assert(pf_sd_card_init(&config) == PF_SD_CARD_INVALID_ARGUMENT);
    config.transport = PF_SD_CARD_TRANSPORT_SDIO_PIO;
    assert(pf_sd_card_init(&config) == PF_SD_CARD_OK);
    initialized = false;
    assert(pf_sd_card_msc_acquire() == PF_SD_CARD_NOT_READY);
    pf_sd_card_diagnostics_t diagnostics;
    pf_sd_card_get_diagnostics(&diagnostics);
    assert(diagnostics.sequence != 0);
    assert(diagnostics.operation == PF_SD_CARD_OPERATION_MSC_ACQUIRE);
    assert(diagnostics.result == PF_SD_CARD_NOT_READY);
    assert(diagnostics.transport_init_stage == 99);
    for (unsigned owner = 0; owner < 2; ++owner) {
        initialized = true;
        assert(set_owner((pf_sd_card_owner_t)owner) == PF_SD_CARD_OK);
        for (unsigned op = 0; op < 3; ++op) {
            /* The facade forwards backend results without extra readiness calls
             * or validation, including uninitialized and invalid requests. */
            for (unsigned result = PF_SD_CARD_OK; result <= PF_SD_CARD_IO_ERROR; ++result) {
                initialized = result != PF_SD_CARD_NOT_READY;
                backend_result = (pf_sd_card_result_t)result;
                expected_block = op == 2 ? 0 : UINT32_MAX;
                expected_count = 0;
                expected_buffer = NULL;
                unsigned before = backend_calls, ready_before = readiness_calls;
                assert(call_operation(op, owner != 0) == backend_result);
                assert(backend_calls == before + 1);
                assert(readiness_calls == ready_before);
                assert(call_operation(op, owner == 0) == PF_SD_CARD_NOT_READY);
                assert(backend_calls == before + 1);
            }
        }
    }
    /* A successful, nonempty transfer must preserve all backend arguments. */
    uint8_t data[1024] = {0};
    backend_result = PF_SD_CARD_OK;
    for (unsigned owner = 0; owner < 2; ++owner) {
        initialized = true;
        assert(set_owner((pf_sd_card_owner_t)owner) == PF_SD_CARD_OK);
        expected_block = 7;
        expected_count = 2;
        expected_buffer = data;
        for (unsigned op = 0; op < 2; ++op) {
            unsigned before = backend_calls;
            assert(call_operation(op, owner != 0) == PF_SD_CARD_OK);
            assert(backend_calls == before + 1);
        }
    }
    assert(pf_sd_card_msc_release() == PF_SD_CARD_OK);
    pf_sd_card_clear_diagnostics();
    pf_sd_card_get_diagnostics(&diagnostics);
    assert(diagnostics.sequence != 0);
    assert(diagnostics.operation == PF_SD_CARD_OPERATION_NONE);
    contract_test = false;
}
int main(void)
{
    check_contracts();
    for (operation = 0; operation < 3; ++operation) {
        for (contender = 0; contender < 3; ++contender) {
            pf_sd_card_config_t config = { .transport = PF_SD_CARD_TRANSPORT_SDIO_PIO };
            assert(pf_sd_card_msc_release() == PF_SD_CARD_OK);
            assert(pf_sd_card_init(&config) == PF_SD_CARD_OK);
            assert(pf_sd_card_msc_acquire() == PF_SD_CARD_OK);
            entered = release_io = contender_started = contender_done = false;
            pthread_t a, b;
            assert(!pthread_create(&a, NULL, io, NULL));
            while (!entered) sched_yield();
            assert(!pthread_create(&b, NULL, other, NULL));
            while (!contender_started) sched_yield();
            for (unsigned i = 0; i < 10000; ++i) sched_yield();
            assert(!contender_done);
            release_io = true;
            assert(!pthread_join(a, NULL)); assert(!pthread_join(b, NULL));
            assert(contender_done);
        }
    }
}
'''
SHIMS = {
    'hardware/spi.h': 'typedef struct spi_inst spi_inst_t;\n',
    'hardware/pio.h': '#include <stdbool.h>\ntypedef void *PIO;\n',
    'FreeRTOS.h': '#include <assert.h>\n#define configASSERT assert\n#define portMAX_DELAY 0\n#define pdTRUE 1\ntypedef int BaseType_t;\n',
    'task.h': '''#include <pthread.h>
extern pthread_mutex_t creation_lock;
#define taskENTER_CRITICAL() pthread_mutex_lock(&creation_lock)
#define taskEXIT_CRITICAL() pthread_mutex_unlock(&creation_lock)
''',
    'semphr.h': '''#include <pthread.h>
#include <assert.h>
typedef pthread_mutex_t StaticSemaphore_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *m) {
    pthread_mutexattr_t a; assert(!pthread_mutexattr_init(&a));
    assert(!pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE));
    assert(!pthread_mutex_init(m, &a)); pthread_mutexattr_destroy(&a); return m;
}
#define xSemaphoreTakeRecursive(m, timeout) (pthread_mutex_lock(m) == 0)
#define xSemaphoreGiveRecursive(m) (pthread_mutex_unlock(m) == 0)
''',
}

def main():
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        for name, body in SHIMS.items():
            p = d / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(body)
        for transport in (0, 1):
            harness = HARNESS
            if transport == 1:
                harness = harness.replace('PF_SD_CARD_TRANSPORT_SDIO_PIO', 'SELECTED_TRANSPORT')
                harness = harness.replace('PF_SD_CARD_TRANSPORT_SPI', 'PF_SD_CARD_TRANSPORT_SDIO_PIO')
                harness = harness.replace('SELECTED_TRANSPORT', 'PF_SD_CARD_TRANSPORT_SPI')
                harness = harness.replace('pf_sdio_pio_', 'pf_sd_spi_')
                harness = harness.replace('pf_sdio_pio_config_t', 'pf_sd_spi_config_t')
            (d / 'test.c').write_text(harness + '\npthread_mutex_t creation_lock = PTHREAD_MUTEX_INITIALIZER;\n')
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-D_XOPEN_SOURCE=700',
                            f'-DPF_CONFIG_SD_CARD_TRANSPORT={transport}',
                            '-Wall', '-Wextra', '-Werror', '-pthread', '-I' + tmp,
                            '-I' + str(ROOT / 'firmware/src'),
                            '-I' + str(ROOT / 'firmware/src/drivers/sd_card'),
                            str(ROOT / 'firmware/src/drivers/sd_card/sd_card.c'),
                            str(d / 'test.c'), '-o', str(d / 'test')], check=True)
            subprocess.run([str(d / 'test')], check=True, timeout=15)
    print('Production facade (SDIO/SPI): owner/configuration/result forwarding and RX/TX/sync versus removal/telemetry/handoff passed')

if __name__ == '__main__':
    main()
