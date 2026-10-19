#include "FreeRTOS.h"
#include "task.h"
#include "pico/stdlib.h"
#include "board/board.h"
#include "drivers/persistent_memory/settings_store.h"
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
    pf_settings_t settings;
    /* Missing or corrupt records load as Defaults (0 = keep the build clock). */
    (void)pf_settings_load_early(&settings);
    (void)pf_system_clock_apply(settings.system_clock_mhz);
}
PICO_RUNTIME_INIT_FUNC_HW(apply_saved_system_clock, "00501");

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    panic("FreeRTOS stack overflow in task %s", task_name);
}

int main(void)
{
    pf_settings_t settings;
    /* Invalid or missing settings load as complete defaults, so boot never depends on them. */
    (void)pf_settings_load_early(&settings);
    (void)pf_board_apply_settings(&settings);
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