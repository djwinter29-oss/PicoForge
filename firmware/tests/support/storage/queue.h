#ifndef STORAGE_TEST_QUEUE_H
#define STORAGE_TEST_QUEUE_H
#include "FreeRTOS.h"
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout);
QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t size,
    uint8_t *storage, StaticQueue_t *control);
#endif
