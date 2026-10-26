#include "FreeRTOS.h"
#include "task.h"
#include "pico/stdlib.h"
#include "drivers/led.h"
#include "drivers/logging.h"
#include "drivers/system.h"
#include "drivers/temperature.h"
#include "app/usb/usb_task.h"

/* SYS_CLK_HZ and the artifact label are the BUILD clock; a saved system clock may replace it at runtime. */
_Static_assert(SYS_CLK_HZ == PF_IMAGE_SYS_CLK_MHZ * 1000000u,
               "Image clock label differs from SDK default clock");

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    panic("FreeRTOS stack overflow in task %s", task_name);
}

int main(void)
{
    (void)pf_system_clock_apply();
    pf_logging_init();
    if (!pf_led_init()) {
        panic("Failed to initialize board LED");
    }
    PF_LOGGING_INFO("boot", "Firmware initialization started");

    pf_temperature_init();

    pf_usb_start();
    PF_LOGGING_INFO("usb", "CDC and HID interfaces initialized");

    vTaskStartScheduler();
    panic("FreeRTOS scheduler returned");
}