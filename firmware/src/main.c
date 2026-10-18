#include "FreeRTOS.h"
#include "task.h"
#include "pico/stdlib.h"
#include "board/board.h"
#include "drivers/temperature.h"
#include "middleware/logging/logging.h"
#include "app/usb/usb_task.h"

_Static_assert(SYS_CLK_HZ == PF_IMAGE_SYS_CLK_MHZ * 1000000u,
               "Image clock label differs from SDK clock; configure PF_SYS_CLK_MHZ explicitly");

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    panic("FreeRTOS stack overflow in task %s", task_name);
}

int main(void)
{
    if (!pf_board_init()) {
        panic("Failed to initialize board pin assignments");
    }
    pf_log_init();
    PF_LOG_INFO("boot", "Firmware initialization started");

    pf_temperature_init();

    pf_usb_start();
    PF_LOG_INFO("usb", "CDC and HID interfaces initialized");

    vTaskStartScheduler();
    panic("FreeRTOS scheduler returned");
}