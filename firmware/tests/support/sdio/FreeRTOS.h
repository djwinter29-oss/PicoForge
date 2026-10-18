#ifndef PF_TEST_SDIO_FREERTOS_H
#define PF_TEST_SDIO_FREERTOS_H

#include <assert.h>

typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef void *TaskHandle_t;

#define pdFALSE 0
#define pdTRUE 1
#define taskSCHEDULER_RUNNING 1
#define PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY 0u
#define PICO_DEFAULT_IRQ_PRIORITY 128u
#define taskENTER_CRITICAL() pf_test_sdio_enter_critical()
#define taskEXIT_CRITICAL() pf_test_sdio_exit_critical()
#define configASSERT assert
#define taskENTER_CRITICAL_FROM_ISR() pf_test_sdio_enter_critical_from_isr()
#define taskEXIT_CRITICAL_FROM_ISR(saved) pf_test_sdio_exit_critical_from_isr(saved)
#define portYIELD_FROM_ISR(awakened) ((void)(awakened))

void pf_test_sdio_enter_critical(void);
void pf_test_sdio_exit_critical(void);
UBaseType_t pf_test_sdio_enter_critical_from_isr(void);
void pf_test_sdio_exit_critical_from_isr(UBaseType_t saved);

#endif
