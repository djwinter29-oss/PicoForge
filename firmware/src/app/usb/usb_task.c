#include "FreeRTOS.h"
#include "task.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "usb_hid.h"
#include "usb_msc.h"
#include "usb_task.h"
#include "app/cli/cli.h"
#include "app/storage/storage_worker.h"

#include <string.h>

#define USB_TASK_STACK_SIZE (1024U)
#define USB_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)
#define USB_CLI_TX_SIZE 2048U
#define USB_CLI_OUTPUT_RESERVE 1024U
#define USB_CLI_RX_BUDGET 32U
#define USB_RECONNECT_DETACH_MS 250U
#define USB_HID_REPORT_INTERVAL (pdMS_TO_TICKS(250U))
#define USB_HID_MEMORY_REPORT_INTERVAL (pdMS_TO_TICKS(1000U))
#define USB_HID_MSC_REPORT_INTERVAL (pdMS_TO_TICKS(500U))
#define USB_HID_SELF_TEST_INTERVAL (pdMS_TO_TICKS(500U))
#define USB_HID_FILESYSTEM_INTERVAL (pdMS_TO_TICKS(1000U))

static StaticTask_t usb_task_control_block;
static StackType_t usb_task_stack[USB_TASK_STACK_SIZE];
static uint8_t hid_sequence;
static uint8_t memory_sequence;
static uint8_t self_test_sequence;
static uint8_t filesystem_sequence;
static char cli_tx[USB_CLI_TX_SIZE];
static uint32_t cli_tx_head;
static uint32_t cli_tx_tail;
static uint32_t cli_tx_count;
static bool cli_connected;
static uint32_t cli_session;

static void storage_command(EmbeddedCli *instance, char *args, void *context)
{
    (void)instance;
    const char *family = context;
    if (strcmp(family, "sd") == 0 && args != NULL &&
        strcmp(args, "recover --confirm") == 0) {
        pf_usb_msc_request_recovery();
        pf_cli_print("Fault recovery requested, not clean eject. Data may be lost; inspect before fs mount. Check UART recovery result and sd status / sd diagnostics after reconnect (selected media).");
        return;
    }
    if (!pf_storage_worker_submit(family, args != NULL ? args : "",
                                  pf_usb_msc_is_enabled(), cli_session)) {
        pf_cli_print("Storage busy or command too long; wait for completion and retry.");
    } else {
        pf_cli_print("Storage command queued.");
    }
}

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
        pf_storage_worker_cancel(cli_session);
        pf_storage_worker_reply_t reply;
        while (pf_storage_worker_poll(&reply)) {
        }
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
        if (!pf_cli_add_binding((CliCommandBinding){
                "sd", "sd status | sd info | sd profile [status|on|off|reset] | sd diagnostics [status|clear] | sd recover --confirm", false,
                "sd", storage_command
            }) || !pf_cli_add_binding((CliCommandBinding){
                "fs", "fs mount|unmount|format --confirm|ls [path]|stat <path>|cat <path> [limit]|df\r\n"
                      "fs mkdir <path>|write <path> <text>|append <path> <text>\r\n"
                      "fs mv <source> <destination>|rm <path> --confirm", false,
                "fs", storage_command
            })) {
            panic("Failed to register storage CLI commands");
        }
        ++cli_session;
        cli_connected = true;
        pf_cli_process();
    }

    cli_drain_output();
    if ((USB_CLI_TX_SIZE - cli_tx_count) >= USB_CLI_OUTPUT_RESERVE) {
        pf_storage_worker_reply_t reply;
        if (pf_storage_worker_poll(&reply) && !reply.done && (reply.session == cli_session)) {
            pf_cli_print(reply.line);
        }
    }
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
    TickType_t last_self_test_report = last_hid_report;
    TickType_t last_filesystem_report = last_hid_report;

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

            if (pf_hid_build_memory_report(&report, memory_sequence) &&
                tud_hid_report(PF_HID_MEMORY_REPORT_ID, &report, sizeof(report))) {
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

        if (tud_hid_ready() &&
            ((TickType_t)(now - last_self_test_report) >= USB_HID_SELF_TEST_INTERVAL)) {
            pf_hid_self_test_status_t report;
            pf_hid_build_self_test_report(&report, self_test_sequence);
            if (tud_hid_report(PF_HID_SELF_TEST_REPORT_ID, &report, sizeof(report))) {
                ++self_test_sequence;
                last_self_test_report = now;
            }
        }

        if (tud_hid_ready() &&
            ((TickType_t)(now - last_filesystem_report) >= USB_HID_FILESYSTEM_INTERVAL)) {
            pf_hid_filesystem_report_t report;
            pf_hid_build_filesystem_report(&report, filesystem_sequence);
            if (tud_hid_report(PF_HID_FILESYSTEM_REPORT_ID, &report, sizeof(report))) {
                ++filesystem_sequence;
                last_filesystem_report = now;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1U));
    }
}

void pf_usb_start(void)
{
    if (!pf_storage_worker_init()) {
        panic("Failed to create storage worker");
    }
    if (!tusb_init()) {
        panic("Failed to initialize TinyUSB");
    }
    /* A debugger or watchdog reset leaves the host on the old address; a visible detach forces re-enumeration. */
    tud_disconnect();
    /* The scheduler is not running yet; do not depend on alarm/RTOS wakeups here. */
    busy_wait_ms(USB_RECONNECT_DETACH_MS);
    tud_connect();

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