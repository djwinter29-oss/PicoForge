#include "FreeRTOS.h"
#include "task.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "hid.h"
#include "msc.h"
#include "usb_task.h"
#include "middleware/cli/cli.h"

#define USB_TASK_STACK_SIZE (1024U)
#define USB_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)
#define USB_CLI_TX_SIZE 2048U
#define USB_CLI_OUTPUT_RESERVE 1024U
#define USB_CLI_RX_BUDGET 32U
#define USB_HID_REPORT_INTERVAL (pdMS_TO_TICKS(250U))
#define USB_HID_MEMORY_REPORT_INTERVAL (pdMS_TO_TICKS(1000U))
#define USB_HID_MSC_REPORT_INTERVAL (pdMS_TO_TICKS(500U))

static StaticTask_t usb_task_control_block;
static StackType_t usb_task_stack[USB_TASK_STACK_SIZE];
static uint8_t hid_sequence;
static uint8_t memory_sequence;
static char cli_tx[USB_CLI_TX_SIZE];
static uint32_t cli_tx_head;
static uint32_t cli_tx_tail;
static uint32_t cli_tx_count;
static bool cli_connected;

static void cli_write_character(char character, void *context)
{
    (void)context;
    if (cli_tx_count == USB_CLI_TX_SIZE) {
        panic("CDC CLI output exceeded its reserved buffer");
    }
    cli_tx[cli_tx_head] = character;
    cli_tx_head = (cli_tx_head + 1u) % USB_CLI_TX_SIZE;
    ++cli_tx_count;
}

static void cli_drain_output(void)
{
    uint32_t available = tud_cdc_write_available();

    while ((cli_tx_count > 0u) && (available > 0u)) {
        uint32_t size = USB_CLI_TX_SIZE - cli_tx_tail;
        if (size > cli_tx_count) {
            size = cli_tx_count;
        }
        if (size > available) {
            size = available;
        }
        uint32_t written = tud_cdc_write(cli_tx + cli_tx_tail, size);
        if (written == 0u) {
            break;
        }
        cli_tx_tail = (cli_tx_tail + written) % USB_CLI_TX_SIZE;
        cli_tx_count -= written;
        available -= written;
    }
    (void)tud_cdc_write_flush();
}

static void cdc_cli_task(void)
{
    if (!tud_cdc_connected()) {
        cli_connected = false;
        cli_tx_head = 0u;
        cli_tx_tail = 0u;
        cli_tx_count = 0u;
        tud_cdc_read_flush();
        tud_cdc_write_clear();
        return;
    }
    if (!cli_connected) {
        if (!pf_cli_init(cli_write_character, NULL)) {
            panic("Failed to initialize CDC CLI");
        }
        cli_connected = true;
        pf_cli_process();
    }

    cli_drain_output();
    for (uint32_t count = 0u; count < USB_CLI_RX_BUDGET; ++count) {
        if ((tud_cdc_available() == 0u) ||
            ((USB_CLI_TX_SIZE - cli_tx_count) < USB_CLI_OUTPUT_RESERVE)) {
            break;
        }
        char character;
        if (tud_cdc_read(&character, 1u) != 1u) {
            break;
        }
        pf_cli_receive_char(character);
        pf_cli_process();
    }
    cli_drain_output();
}

static void usb_task(void *context)
{
    (void)context;
    TickType_t last_hid_report = xTaskGetTickCount();
    TickType_t last_memory_report = last_hid_report;
    TickType_t last_msc_report = last_hid_report;

    while (true) {
        TickType_t now = xTaskGetTickCount();

        tud_task();
        pf_usb_msc_task();
        cdc_cli_task();

        if (tud_hid_ready() &&
            ((TickType_t)(now - last_hid_report) >= USB_HID_REPORT_INTERVAL)) {
            pf_hid_status_report_t report;

            pf_hid_build_status_report(&report, hid_sequence);
            if (tud_hid_report(PF_HID_STATUS_REPORT_ID, &report, sizeof(report))) {
                ++hid_sequence;
                last_hid_report = now;
            }
        }

        if (tud_hid_ready() &&
            ((TickType_t)(now - last_memory_report) >= USB_HID_MEMORY_REPORT_INTERVAL)) {
            pf_hid_memory_report_t report;

            pf_hid_build_memory_report(&report, memory_sequence);
            if (tud_hid_report(PF_HID_MEMORY_REPORT_ID, &report, sizeof(report))) {
                ++memory_sequence;
                last_memory_report = now;
            }
        }

        if (tud_hid_ready() &&
            ((TickType_t)(now - last_msc_report) >= USB_HID_MSC_REPORT_INTERVAL)) {
            pf_hid_msc_status_report_t report;

            pf_hid_build_msc_status_report(&report);
            if (tud_hid_report(PF_HID_MSC_COMMAND_REPORT_ID, &report, sizeof(report))) {
                last_msc_report = now;
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