#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/sdio")

#define PF_CONFIG_SD_CARD_INTERRUPT_WAIT 1
#include "FreeRTOS.h"
#include "hardware/irq.h"
#include "task.h"

#include <stdint.h>

#include "../../../../src/drivers/sd_card/ports/freertos/sd_card_wait.c"

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
static bool early_irq;
static bool late_irq;

static bool fake_pio_interrupt_get(PIO pio, uint32_t flag)
{
    return ((pf_test_sdio_irq_pio_t *)pio)->flag[flag];
}

static void fake_pio_set_irq0_source_enabled(PIO pio, pio_interrupt_source_t source,
                                             bool enabled)
{
    ((pf_test_sdio_irq_pio_t *)pio)->source[source] = enabled;
}

void pf_sdio_pio_set_wait_hook(void (*hook)(PIO, uint32_t))
{
    wait_hook = hook;
}

void setUp(void)
{
    installed_handler = NULL;
    wait_hook = NULL;
    active_pio = NULL;
    scheduler_state = 0u;
    notifications = blocked = notified = critical_depth = 0u;
    early_irq = late_irq = false;
}

void tearDown(void) {}

void pf_test_sdio_enter_critical(void)
{
    ++critical_depth;
}

void pf_test_sdio_exit_critical(void)
{
    TEST_ASSERT_EQUAL_UINT(1u, critical_depth);
    --critical_depth;
    if (early_irq) {
        early_irq = false;
        ((pf_test_sdio_irq_pio_t *)active_pio)->flag[1] = true;
        installed_handler();
    }
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
    pf_sd_card_config_t config = {
        .transport = PF_SD_CARD_TRANSPORT_SPI,
        .bus.sdio_pio.pio = (PIO)&pio
    };

    pf_sd_card_port_init(NULL);
    pf_sd_card_port_init(&config);
    TEST_ASSERT_NULL(wait_hook);

    config.transport = PF_SD_CARD_TRANSPORT_SDIO_PIO;
    pf_sd_card_port_init(&config);
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
    TEST_ASSERT_FALSE(pio.source[1]);
    TEST_ASSERT_FALSE(pio.flag[1]);

    early_irq = true;
    wait_hook((PIO)&pio, 1u);
    TEST_ASSERT_EQUAL_UINT(1u, notified);
    TEST_ASSERT_TRUE(pio.flag[1]);
    TEST_ASSERT_FALSE(pio.source[1]);
    TEST_ASSERT_NULL(waiting_task);
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
