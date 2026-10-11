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
    if (!pf_board_init()) {
        panic("Failed to initialize board pin assignments");
    }
    pf_log_init();
    PF_LOG_INFO("boot", "Firmware initialization started");

    pf_temperature_init();

    pf_sd_card_config_t sd_card_config;
    if (pf_board_get_sd_card_config(&sd_card_config)) {
        pf_sd_card_result_t sd_card_result = pf_sd_card_init(&sd_card_config);
        if (sd_card_result == PF_SD_CARD_OK) {
            PF_LOG_INFO("storage", "SD card initialized");
        } else {
            PF_LOG_WARNING("storage", "SD card unavailable (result %u)",
                           (unsigned)sd_card_result);
        }
    }

    pf_usb_start();
    PF_LOG_INFO("usb", "CDC and HID interfaces initialized");

    vTaskStartScheduler();
    panic("FreeRTOS scheduler returned");
}