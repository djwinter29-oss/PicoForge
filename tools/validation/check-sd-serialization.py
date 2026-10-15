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
static atomic_bool entered, release_io, contender_started, contender_done;
static bool initialized;
pf_sd_card_result_t pf_sdio_pio_init(const pf_sdio_pio_config_t *c)
{ (void)c; initialized = true; return PF_SD_CARD_OK; }
bool pf_sdio_pio_is_initialized(void) { return initialized; }
uint32_t pf_sdio_pio_block_count(void) { return 100; }
pf_sd_card_result_t pf_sdio_pio_read_blocks(uint32_t b, void *p, uint32_t n)
{
    (void)b; (void)p; (void)n;
    atomic_store(&entered, true);
    while (!atomic_load(&release_io)) sched_yield();
    assert(!atomic_load(&contender_done));
    return PF_SD_CARD_OK;
}
pf_sd_card_result_t pf_sdio_pio_write_blocks(uint32_t b, const void *p, uint32_t n)
{ return pf_sdio_pio_read_blocks(b, (void *)p, n); }
pf_sd_card_result_t pf_sdio_pio_sync(void)
{ return pf_sdio_pio_read_blocks(0, NULL, 0); }
void pf_sdio_pio_get_info(pf_sd_card_info_t *p) { p->initialized = initialized; }
void pf_sdio_pio_confirm_card_removed(void) { initialized = false; }
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
    else assert(pf_sd_card_set_owner(PF_SD_CARD_OWNER_FIRMWARE) == PF_SD_CARD_OK);
    atomic_store(&contender_done, true); return NULL;
}
int main(void)
{
    for (operation = 0; operation < 3; ++operation) {
        for (contender = 0; contender < 3; ++contender) {
            pf_sd_card_config_t config = { .transport = PF_SD_CARD_TRANSPORT_SDIO_PIO };
            assert(pf_sd_card_set_owner(PF_SD_CARD_OWNER_FIRMWARE) == PF_SD_CARD_OK);
            assert(pf_sd_card_init(&config) == PF_SD_CARD_OK);
            assert(pf_sd_card_set_owner(PF_SD_CARD_OWNER_USB_MSC) == PF_SD_CARD_OK);
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
        (d / 'test.c').write_text(HARNESS + '\npthread_mutex_t creation_lock = PTHREAD_MUTEX_INITIALIZER;\n')
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-D_XOPEN_SOURCE=700',
                        '-Wall', '-Wextra', '-Werror', '-pthread', '-I' + tmp,
                        '-I' + str(ROOT / 'firmware/src'),
                        '-I' + str(ROOT / 'firmware/src/drivers/sd_card'),
                        str(ROOT / 'firmware/src/drivers/sd_card/sd_card.c'),
                        str(d / 'test.c'), '-o', str(d / 'test')], check=True)
        subprocess.run([str(d / 'test')], check=True, timeout=15)
    print('Production facade: RX/TX/sync versus removal/telemetry/handoff passed')

if __name__ == '__main__':
    main()
