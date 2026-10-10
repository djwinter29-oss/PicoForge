#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "pico/stdlib.h"

#define HELLO_TASK_STACK_SIZE (512U)
#define HELLO_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)

static StaticTask_t hello_task_control_block;
static StackType_t hello_task_stack[HELLO_TASK_STACK_SIZE];

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    panic("FreeRTOS stack overflow in task %s", task_name);
}

static void hello_task(void *context)
{
    (void)context;

    while (true) {
        printf("Hello from PicoForge!\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

int main(void)
{
    stdio_init_all();

    if (xTaskCreateStatic(hello_task,
                          "Hello",
                          HELLO_TASK_STACK_SIZE,
                          NULL,
                          HELLO_TASK_PRIORITY,
                          hello_task_stack,
                          &hello_task_control_block) == NULL) {
        panic("Failed to create the Hello task");
    }

    vTaskStartScheduler();
    panic("FreeRTOS scheduler returned");
}