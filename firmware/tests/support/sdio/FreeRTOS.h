#ifndef PF_TEST_SDIO_FREERTOS_H
#define PF_TEST_SDIO_FREERTOS_H

typedef int BaseType_t;
typedef void *TaskHandle_t;

#define pdFALSE 0
#define pdTRUE 1
#define taskSCHEDULER_RUNNING 1
#define PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY 0u
#define PICO_DEFAULT_IRQ_PRIORITY 128u
#define taskENTER_CRITICAL() pf_test_sdio_enter_critical()
#define taskEXIT_CRITICAL() pf_test_sdio_exit_critical()
#define portYIELD_FROM_ISR(awakened) ((void)(awakened))

void pf_test_sdio_enter_critical(void);
void pf_test_sdio_exit_critical(void);

#endif
