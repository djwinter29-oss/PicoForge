#include "FreeRTOS.h"
#include "task.h"
#include "pico/stdlib.h"
#include "usb/usb_task.h"

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    panic("FreeRTOS stack overflow in task %s", task_name);
}

int main(void)
{
    picoforge_usb_start();

    vTaskStartScheduler();
    panic("FreeRTOS scheduler returned");
}