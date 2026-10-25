#include "FreeRTOS.h"
#include "task.h"
#include "pico/stdlib.h"
#include "drivers/boot_config.h"
#include "drivers/led.h"
#include "drivers/monitor_uart.h"
#include "drivers/system.h"
#include "pico/runtime.h"
#include "drivers/temperature.h"
#include "middleware/logging/logging.h"
#include "app/usb/usb_task.h"

/* SYS_CLK_HZ and the artifact label are the BUILD clock; a saved system clock may replace it at runtime. */
_Static_assert(SYS_CLK_HZ == PF_IMAGE_SYS_CLK_MHZ * 1000000u,
               "Image clock label differs from SDK clock; configure PF_SYS_CLK_MHZ explicitly");

/* Runs after the SDK clock init (00500) and before peripheral resets are released (00600), stdio and FreeRTOS. */
static void apply_saved_system_clock(void)
{
    /* First boot-config access: loads the record before the scheduler; missing or corrupt records give 0. */
    (void)pf_system_clock_apply(pf_sysclk_profile_mhz(pf_boot_config_sysclk()));
}
PICO_RUNTIME_INIT_FUNC_HW(apply_saved_system_clock, "00501");

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    panic("FreeRTOS stack overflow in task %s", task_name);
}

int main(void)
{
    pf_monitor_uart_init();
    if (!pf_led_init()) {
        panic("Failed to initialize board LED");
    }
    pf_log_init();
    PF_LOG_INFO("boot", "Firmware initialization started");

    pf_temperature_init();

    pf_usb_start();
    PF_LOG_INFO("usb", "CDC and HID interfaces initialized");

    vTaskStartScheduler();
    panic("FreeRTOS scheduler returned");
}