#include "FreeRTOS.h"
#include "task.h"
#include "pico/stdlib.h"
#include "board/board.h"
#include "drivers/led.h"
#include "drivers/temperature.h"
#include "usb/usb_task.h"

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    panic("FreeRTOS stack overflow in task %s", task_name);
}

int main(void)
{
    picoforge_board_led_config_t led_config;
    picoforge_temperature_init();
    if (picoforge_board_get_led_config(&led_config)) {
        (void)picoforge_led_init(led_config.gpio, led_config.active_high);
    }

    picoforge_usb_start();

    vTaskStartScheduler();
    panic("FreeRTOS scheduler returned");
}