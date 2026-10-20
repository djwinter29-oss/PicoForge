#ifndef STORAGE_TEST_TASK_H
#define STORAGE_TEST_TASK_H
#include "FreeRTOS.h"
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
TaskHandle_t xTaskCreateStatic(void (*entry)(void *), const char *name,
    uint32_t depth, void *argument, UBaseType_t priority, StackType_t *stack,
    StaticTask_t *control);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
uint64_t ulTaskGetIdleRunTimeCounter(void);
#endif
