#ifndef PF_TEST_SDIO_TASK_H
#define PF_TEST_SDIO_TASK_H

#include "FreeRTOS.h"
#include <stdint.h>

unsigned xTaskGetSchedulerState(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskNotifyGiveIndexedFromISR(TaskHandle_t task, unsigned index, BaseType_t *awakened);
uint32_t ulTaskNotifyTakeIndexed(unsigned index, BaseType_t clear, unsigned ticks);

#endif
