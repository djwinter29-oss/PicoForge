#include "sd_card_wait.h"
#include "config/config.h"

#if PF_CONFIG_SD_CARD_INTERRUPT_WAIT
#include "FreeRTOS.h"
#include "task.h"
#include "hardware/irq.h"
#include "hardware/pio.h"

static PIO wait_pio;
static TaskHandle_t volatile waiting_task;
static uint32_t waiting_flag;
static bool handler_installed;

static pio_interrupt_source_t completion_source(uint32_t flag)
{
    return (pio_interrupt_source_t)(pis_interrupt0 + flag);
}

static void completion_irq(void)
{
    if (waiting_task != NULL && pio_interrupt_get(wait_pio, waiting_flag)) {
        pio_set_irq0_source_enabled(wait_pio, completion_source(waiting_flag), false);
        BaseType_t awakened = pdFALSE;
        vTaskNotifyGiveIndexedFromISR(waiting_task, 1u, &awakened);
        portYIELD_FROM_ISR(awakened);
    }
}

static void wait_completion(PIO pio, uint32_t flag)
{
    if (pio != wait_pio || (flag != 1u && flag != 2u) ||
        xTaskGetSchedulerState() != taskSCHEDULER_RUNNING ||
        pio_interrupt_get(pio, flag)) {
        return;
    }
    (void)ulTaskNotifyTakeIndexed(1u, pdTRUE, 0u);
    taskENTER_CRITICAL();
    waiting_task = xTaskGetCurrentTaskHandle();
    waiting_flag = flag;
    pio_set_irq0_source_enabled(pio, completion_source(flag), true);
    taskEXIT_CRITICAL();
    (void)ulTaskNotifyTakeIndexed(1u, pdTRUE, 1u);
    taskENTER_CRITICAL();
    pio_set_irq0_source_enabled(pio, completion_source(flag), false);
    waiting_task = NULL;
    taskEXIT_CRITICAL();
}
#endif

void pf_sd_card_wait_init(const pf_sd_card_config_t *config)
{
#if PF_CONFIG_SD_CARD_INTERRUPT_WAIT
    if (config == NULL || config->transport != PF_SD_CARD_TRANSPORT_SDIO_PIO ||
        handler_installed) {
        return;
    }
    wait_pio = config->bus.sdio_pio.pio;
    uint irq = (uint)pio_get_irq_num(wait_pio, 0u);
    pio_set_irq0_source_enabled(wait_pio, pis_interrupt1, false);
    pio_set_irq0_source_enabled(wait_pio, pis_interrupt2, false);
    irq_add_shared_handler(irq, completion_irq, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_priority(irq, PICO_DEFAULT_IRQ_PRIORITY);
    irq_set_enabled(irq, true);
    handler_installed = true;
    pf_sdio_pio_set_wait_hook(wait_completion);
#else
    (void)config;
#endif
}