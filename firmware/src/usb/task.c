#include "FreeRTOS.h"
#include "task.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "hid.h"
#include "usb_task.h"

#define USB_TASK_STACK_SIZE (1024U)
#define USB_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)
#define USB_CDC_REPORT_INTERVAL (pdMS_TO_TICKS(1000U))
#define USB_HID_REPORT_INTERVAL (pdMS_TO_TICKS(250U))

static StaticTask_t usb_task_control_block;
static StackType_t usb_task_stack[USB_TASK_STACK_SIZE];
static uint8_t hid_sequence;

static void usb_task(void *context)
{
    (void)context;
    static const char cdc_greeting[] = "PicoForge CDC ready\r\n";
    TickType_t last_cdc_report = xTaskGetTickCount();
    TickType_t last_hid_report = last_cdc_report;

    while (true) {
        TickType_t now = xTaskGetTickCount();

        tud_task();

        if (tud_cdc_connected() &&
            ((TickType_t)(now - last_cdc_report) >= USB_CDC_REPORT_INTERVAL) &&
            (tud_cdc_write_available() >= (sizeof(cdc_greeting) - 1U))) {
            (void)tud_cdc_write(cdc_greeting, (uint32_t)(sizeof(cdc_greeting) - 1U));
            tud_cdc_write_flush();
            last_cdc_report = now;
        }

        if (tud_hid_ready() &&
            ((TickType_t)(now - last_hid_report) >= USB_HID_REPORT_INTERVAL)) {
            pf_hid_status_report_t report;

            pf_hid_build_status_report(&report, hid_sequence);
            if (tud_hid_report(PF_HID_STATUS_REPORT_ID, &report, sizeof(report))) {
                ++hid_sequence;
                last_hid_report = now;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1U));
    }
}

void pf_usb_start(void)
{
    if (!tusb_init()) {
        panic("Failed to initialize TinyUSB");
    }

    if (xTaskCreateStatic(usb_task,
                          "USB",
                          USB_TASK_STACK_SIZE,
                          NULL,
                          USB_TASK_PRIORITY,
                          usb_task_stack,
                          &usb_task_control_block) == NULL) {
        panic("Failed to create the USB task");
    }
}