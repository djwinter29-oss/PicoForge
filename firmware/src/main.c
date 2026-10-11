#include "FreeRTOS.h"
#include "task.h"
#include "pico/stdlib.h"
#include "board/board.h"
#include "drivers/led.h"
#include "drivers/temperature.h"
#include "middleware/logging/logging.h"
#include "usb/usb_task.h"

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    panic("FreeRTOS stack overflow in task %s", task_name);
}

int main(void)
{
    pf_board_led_config_t led_config;

    if (!pf_log_init()) {
        panic("Failed to initialize UART monitor logging");
    }
    PF_LOG_INFO("boot", "Firmware initialization started");

    pf_temperature_init();
    if (pf_board_get_led_config(&led_config)) {
        (void)pf_led_init(led_config.gpio, led_config.active_high);
    }

    pf_usb_start();
    PF_LOG_INFO("usb", "CDC and HID interfaces initialized");

    vTaskStartScheduler();
    panic("FreeRTOS scheduler returned");
}