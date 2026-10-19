#ifndef FREERTOS_H
#define FREERTOS_H

#include <stdint.h>

typedef unsigned UBaseType_t;
typedef int BaseType_t;
typedef uint32_t TickType_t;

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

/* Host model: tests set these to place the caller in a task, ISR, or pre-scheduler context. */
extern int pf_test_in_isr;
BaseType_t xPortIsInsideInterrupt(void);

#endif
