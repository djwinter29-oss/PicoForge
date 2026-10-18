#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/sdio")

#include "sdio_wait_fixture.h"

#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

#include "../../../../src/drivers/storage/sdio_pio/sdio_wait_freertos.c"

typedef struct {
    bool flag[3];
    bool source[3];
} pf_test_sdio_irq_pio_t;

static void (*installed_handler)(void);
static void (*wait_hook)(PIO pio, uint32_t flag);
static PIO active_pio;
static unsigned scheduler_state;
static unsigned notifications;
static unsigned blocked;
static unsigned notified;
static unsigned critical_depth;
static unsigned isr_critical_depth;
static bool early_irq;
static bool late_irq;
static bool timeout_irq;
static pthread_t timeout_thread;
static pthread_mutex_t kernel_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool irq_attempting;
PIO pio0, pio1;

static void *concurrent_irq(void *context)
{
    (void)context;
    installed_handler();
    return NULL;
}

bool pio_interrupt_get(PIO pio, uint32_t flag)
{
    return ((pf_test_sdio_irq_pio_t *)pio)->flag[flag];
}

static void fake_pio_set_irq0_source_enabled(PIO pio, pio_interrupt_source_t source,
                                             bool enabled)
{
    ((pf_test_sdio_irq_pio_t *)pio)->source[source] = enabled;
}

void setUp(void)
{
    handler_installed = false;
    wait_pio = NULL;
    waiting_task = NULL;
    installed_handler = NULL;
    wait_hook = NULL;
    active_pio = NULL;
    scheduler_state = 0u;
    notifications = blocked = notified = critical_depth = isr_critical_depth = 0u;
    early_irq = late_irq = false;
    timeout_irq = false;
    atomic_store(&irq_attempting, false);
}

void tearDown(void) {}

void pf_test_sdio_enter_critical(void)
{
    TEST_ASSERT_EQUAL_INT(0, pthread_mutex_lock(&kernel_lock));
    ++critical_depth;
    if (timeout_irq && waiting_task != NULL) {
        timeout_irq = false;
        ((pf_test_sdio_irq_pio_t *)active_pio)->flag[waiting_flag] = true;
        TEST_ASSERT_EQUAL_INT(0, pthread_create(&timeout_thread, NULL, concurrent_irq, NULL));
        while (!atomic_load(&irq_attempting)) {
            sched_yield();
        }
    }
}

void pf_test_sdio_exit_critical(void)
{
    TEST_ASSERT_EQUAL_UINT(1u, critical_depth);
    --critical_depth;
    TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&kernel_lock));
    if (early_irq) {
        early_irq = false;
        ((pf_test_sdio_irq_pio_t *)active_pio)->flag[1] = true;
        installed_handler();
    }
}

UBaseType_t pf_test_sdio_enter_critical_from_isr(void)
{
    atomic_store(&irq_attempting, true);
    TEST_ASSERT_EQUAL_INT(0, pthread_mutex_lock(&kernel_lock));
    TEST_ASSERT_EQUAL_UINT(0u, critical_depth);
    TEST_ASSERT_EQUAL_UINT(0u, isr_critical_depth);
    ++isr_critical_depth;
    return 17u;
}

void pf_test_sdio_exit_critical_from_isr(UBaseType_t saved)
{
    TEST_ASSERT_EQUAL_UINT(17u, saved);
    TEST_ASSERT_EQUAL_UINT(1u, isr_critical_depth);
    --isr_critical_depth;
    TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&kernel_lock));
}

unsigned xTaskGetSchedulerState(void)
{
    return scheduler_state;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return (void *)(uintptr_t)1u;
}

void vTaskNotifyGiveIndexedFromISR(TaskHandle_t task, unsigned index, BaseType_t *awakened)
{
    TEST_ASSERT_EQUAL_PTR((void *)(uintptr_t)1u, task);
    TEST_ASSERT_EQUAL_UINT(1u, index);
    TEST_ASSERT_EQUAL_UINT(0u, critical_depth);
    TEST_ASSERT_EQUAL_UINT(1u, isr_critical_depth);
    ++notifications;
    ++notified;
    *awakened = pdTRUE;
}

uint32_t ulTaskNotifyTakeIndexed(unsigned index, BaseType_t clear, unsigned ticks)
{
    TEST_ASSERT_EQUAL_UINT(1u, index);
    TEST_ASSERT_EQUAL_INT(pdTRUE, clear);
    TEST_ASSERT_TRUE(ticks <= 1u);
    if (ticks != 0u) {
        ++blocked;
        if (late_irq) {
            late_irq = false;
            ((pf_test_sdio_irq_pio_t *)active_pio)->flag[2] = true;
            installed_handler();
        }
    }
    uint32_t result = notifications;
    notifications = 0u;
    return result;
}

unsigned pio_get_irq_num(PIO pio, unsigned index)
{
    active_pio = pio;
    TEST_ASSERT_EQUAL_UINT(0u, index);
    return 7u;
}

void pio_set_irq0_source_enabled(PIO pio, pio_interrupt_source_t source, bool enabled)
{
    fake_pio_set_irq0_source_enabled(pio, source, enabled);
}

void irq_add_shared_handler(unsigned irq, void (*handler)(void), unsigned order)
{
    TEST_ASSERT_EQUAL_UINT(7u, irq);
    TEST_ASSERT_EQUAL_UINT(PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY, order);
    installed_handler = handler;
}

void irq_set_priority(unsigned irq, unsigned priority)
{
    TEST_ASSERT_EQUAL_UINT(7u, irq);
    TEST_ASSERT_EQUAL_UINT(PICO_DEFAULT_IRQ_PRIORITY, priority);
}

void irq_set_enabled(unsigned irq, bool enabled)
{
    TEST_ASSERT_EQUAL_UINT(7u, irq);
    TEST_ASSERT_TRUE(enabled);
}

void test_wait_hook_filters_invalid_requests_and_handles_both_irq_races(void)
{
    pf_test_sdio_irq_pio_t pio = {0}, foreign = {0};
    pio0 = (PIO)&pio;
    pio1 = (PIO)&foreign;
    pf_sdio_pio_t transport = { .config.pio = pio0 };

    TEST_ASSERT_FALSE(pf_sdio_pio_enable_wait(NULL));
    TEST_ASSERT_NULL(transport.wait_hook);
    TEST_ASSERT_TRUE(pf_sdio_pio_enable_wait(&transport));
    wait_hook = transport.wait_hook;
    TEST_ASSERT_NOT_NULL(wait_hook);
    TEST_ASSERT_FALSE(pio.source[1]);
    TEST_ASSERT_FALSE(pio.source[2]);

    wait_hook((PIO)&pio, 1u);
    scheduler_state = taskSCHEDULER_RUNNING;
    wait_hook((PIO)&foreign, 1u);
    wait_hook((PIO)&pio, 0u);
    TEST_ASSERT_EQUAL_UINT(0u, blocked);

    notifications = 9u;
    wait_hook((PIO)&pio, 1u);
    TEST_ASSERT_EQUAL_UINT(1u, blocked);
    TEST_ASSERT_EQUAL_UINT(0u, notifications);
    TEST_ASSERT_NULL(waiting_task);
    installed_handler();
    TEST_ASSERT_EQUAL_UINT(0u, notified);
    TEST_ASSERT_FALSE(pio.source[1]);
    TEST_ASSERT_FALSE(pio.flag[1]);

    early_irq = true;
    wait_hook((PIO)&pio, 1u);
    TEST_ASSERT_EQUAL_UINT(1u, notified);
    TEST_ASSERT_TRUE(pio.flag[1]);
    TEST_ASSERT_FALSE(pio.source[1]);
    TEST_ASSERT_NULL(waiting_task);
    installed_handler();
    TEST_ASSERT_EQUAL_UINT(1u, notified);
    unsigned previous_blocked = blocked;
    wait_hook((PIO)&pio, 1u);
    TEST_ASSERT_EQUAL_UINT(previous_blocked, blocked);

    late_irq = true;
    wait_hook((PIO)&pio, 2u);
    TEST_ASSERT_EQUAL_UINT(2u, notified);
    TEST_ASSERT_TRUE(pio.flag[2]);
    TEST_ASSERT_FALSE(pio.source[2]);
    TEST_ASSERT_NULL(waiting_task);
}

void test_irq_on_other_core_cannot_notify_after_timeout_cleanup(void)
{
    pf_test_sdio_irq_pio_t pio = {0};
    pio0 = (PIO)&pio;
    pf_sdio_pio_t transport = { .config.pio = pio0 };
    TEST_ASSERT_TRUE(pf_sdio_pio_enable_wait(&transport));
    scheduler_state = taskSCHEDULER_RUNNING;
    timeout_irq = true;
    transport.wait_hook(pio0, 1u);
    TEST_ASSERT_FALSE(timeout_irq);
    TEST_ASSERT_EQUAL_INT(0, pthread_join(timeout_thread, NULL));
    TEST_ASSERT_EQUAL_UINT(0u, notified);
    TEST_ASSERT_NULL(waiting_task);
    TEST_ASSERT_FALSE(pio.source[1]);
    TEST_ASSERT_EQUAL_UINT(0u, critical_depth);
    TEST_ASSERT_EQUAL_UINT(0u, isr_critical_depth);
}
