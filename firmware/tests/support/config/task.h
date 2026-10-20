#ifndef TASK_H
#define TASK_H

#include "FreeRTOS.h"

#define taskSCHEDULER_SUSPENDED ((BaseType_t)0)
#define taskSCHEDULER_NOT_STARTED ((BaseType_t)1)
#define taskSCHEDULER_RUNNING ((BaseType_t)2)

BaseType_t xTaskGetSchedulerState(void);
UBaseType_t uxTaskGetNumberOfTasks(void);
void vTaskDelay(TickType_t ticks);

#endif
