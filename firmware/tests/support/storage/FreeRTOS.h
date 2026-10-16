#ifndef STORAGE_TEST_FREERTOS_H
#define STORAGE_TEST_FREERTOS_H
#include <assert.h>
#include <stdint.h>
#define configASSERT assert
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
#define tskIDLE_PRIORITY 0u
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
typedef struct { unsigned unused; } StaticTask_t;
typedef struct { unsigned unused; } StaticQueue_t;
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
#endif
