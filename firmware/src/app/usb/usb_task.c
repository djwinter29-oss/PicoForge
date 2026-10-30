/**
 * @file
 * @brief USB service task: CDC CLI pump, HID report scheduling and MSC ownership. Command grammar lives in app/cli.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "platform/system.h"
#include "usb_hid.h"
#include "platform/logging.h"
#include "usb_msc.h"
#include "usb_task.h"
#include "app/cli/cli.h"
#include "app/cli/cli_storage.h"
#include "app/storage/storage_worker.h"

#include <string.h>

#define USB_TASK_STACK_SIZE (1024U) /**< USB task stack in words. */
#define USB_TASK_PRIORITY (tskIDLE_PRIORITY + 1U) /**< One above idle; the storage worker does the slow work. */
#define USB_CLI_TX_SIZE 2048U /**< CDC output ring size in bytes. */
#define USB_CLI_OUTPUT_RESERVE 1024U /**< Free ring space required before accepting input or worker replies. */
#define USB_CLI_RX_BUDGET 32U /**< Maximum CDC input characters handled per task cycle. */
#define USB_CLI_STALL_TIMEOUT (pdMS_TO_TICKS(5000U)) /**< Host-side CDC silence before the session is cut off. */
#define USB_RECONNECT_DETACH_MS 250U /**< Boot-time detach so the host re-enumerates after a debugger reset. */
#define USB_HID_REPORT_INTERVAL (pdMS_TO_TICKS(250U)) /**< Telemetry input report period. */
#define USB_HID_STORAGE_REPORT_INTERVAL (pdMS_TO_TICKS(500U)) /**< Storage input report period. */
#define USB_HID_SELF_TEST_INTERVAL (pdMS_TO_TICKS(500U)) /**< Benchmark input report period. */
#define USB_STACK_SAMPLE_INTERVAL (pdMS_TO_TICKS(30000u)) /**< Period of the stack high-water log line. */

/** CDC "storage" and "fs" command help; the grammar itself is implemented in app/cli/cli_storage. */
#define USB_CLI_STORAGE_HELP "storage status | storage recover --confirm"
#define USB_CLI_FS_HELP \
    "fs mount|unmount|format --confirm|ls [path]|stat <path>|cat <path> [limit]|df\r\n" \
    "fs mkdir <path>|write <path> <text>|append <path> <text>\r\n" \
    "fs mv <source> <destination>|rm <path> --confirm"

static StaticTask_t usb_task_control_block; /**< Static TCB for the USB task. */
static StackType_t usb_task_stack[USB_TASK_STACK_SIZE]; /**< Static stack for the USB task. */
static uint8_t hid_sequence; /**< Telemetry report sequence number. */
static uint8_t storage_sequence; /**< Storage report sequence number. */
static uint8_t self_test_sequence; /**< Benchmark report sequence number. */
static char cli_tx[USB_CLI_TX_SIZE]; /**< CDC output ring; producers are the CLI writer and the USB task only. */
static uint32_t cli_tx_head; /**< Ring write index. */
static uint32_t cli_tx_tail; /**< Ring read index. */
static uint32_t cli_tx_count; /**< Bytes queued in the ring. */
static bool cli_connected; /**< A CDC session (DTR) is open and the CLI is initialized. */
static uint32_t cli_session; /**< Session counter; worker replies from older sessions are dropped. */
static TickType_t cli_last_progress; /**< Tick of the last CDC drain progress or idle moment. */
static bool cli_stall_reported; /**< The timeout notice was already queued for this session. */
static bool cli_stalled; /**< Timed-out session; drain completion until DTR closes. */

static void storage_command(EmbeddedCli *instance, char *args, void *context)
{
    (void)instance;
    const char *family = context;
    if (strcmp(family, "storage") == 0 && args != NULL &&
        strcmp(args, "recover --confirm") == 0) {
        pf_usb_msc_request_recovery();
        pf_cli_print("Fault recovery requested, not clean eject. Data may be lost; inspect before fs mount. "
                     "Check UART recovery result and storage status after reconnect.");
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
        /* Producers reserve space before printing; overflow means a missing reservation, not a runtime condition. */
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
        cli_last_progress = xTaskGetTickCount();
        available -= written;
    }
    (void)tud_cdc_write_flush();
}

/** @brief Drop every queued worker reply; used when the session is closed or its output is truncated. */
static void discard_worker_replies(void)
{
    pf_storage_worker_reply_t reply;
    while (pf_storage_worker_poll(&reply)) {
    }
}

/** @brief Close the CLI session: cancel worker output, discard replies and reset the CDC buffers. */
static void cdc_cli_reset(void)
{
    pf_storage_worker_cancel(cli_session);
    discard_worker_replies();
    cli_connected = false;
    cli_stalled = cli_stall_reported = false;
    cli_tx_head = 0u;
    cli_tx_tail = 0u;
    cli_tx_count = 0u;
    tud_cdc_read_flush();
    tud_cdc_write_clear();
}

/** @brief Open a new CLI session: initialize the CLI, register the storage commands and bump the session. */
static void cdc_cli_connect(void)
{
    if (!pf_cli_init(cli_write_character, NULL)) {
        panic("Failed to initialize CDC CLI");
    }
    if (!pf_cli_add_binding((CliCommandBinding){
            "storage", USB_CLI_STORAGE_HELP, false,
            "storage", storage_command
        }) || !pf_cli_add_binding((CliCommandBinding){
            "fs", USB_CLI_FS_HELP, false,
            "fs", storage_command
        })) {
        panic("Failed to register storage CLI commands");
    }
    ++cli_session;
    cli_connected = true;
    cli_last_progress = xTaskGetTickCount();
    pf_cli_process();
}

static void cdc_cli_task(void)
{
    if (!tud_cdc_connected()) {
        cdc_cli_reset();
        return;
    }
    if (!cli_connected) {
        cdc_cli_connect();
    }

    cli_drain_output();
    if (!cli_stalled && (cli_tx_count > 0u) &&
        ((TickType_t)(xTaskGetTickCount() - cli_last_progress) >= USB_CLI_STALL_TIMEOUT)) {
        cli_stalled = true;
        pf_storage_worker_cancel(cli_session);
    }
    if (cli_stalled) {
        discard_worker_replies();
        if (!cli_stall_reported && (USB_CLI_TX_SIZE - cli_tx_count) >= USB_CLI_OUTPUT_RESERVE) {
            pf_cli_print("CDC output timed out; command output truncated. Changes may have completed. "
                         "Close/reopen DTR; inspect before retrying.");
            /* Keep the notice ordered after buffered output and emit it only once. */
            cli_last_progress = xTaskGetTickCount();
            cli_stall_reported = true;
        }
        tud_cdc_read_flush();
        cli_drain_output();
        return;
    }
    if (cli_tx_count == 0u) {
        cli_last_progress = xTaskGetTickCount();
    }
    /* ponytail: one worker reply per 1 ms cycle caps CLI output near 1000 lines/s, far above CDC full-speed
     * throughput for these short lines; poll in a bounded loop if long listings ever become the bottleneck. */
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

/** @brief Keep control transfers serviced during the acknowledgement grace period before resetting. */
static void reboot_after_usb_flush(void)
{
    const TickType_t started = xTaskGetTickCount();
    do {
        tud_task_ext(0u, false);
        pf_usb_msc_task();
        vTaskDelay(pdMS_TO_TICKS(1u));
    } while ((TickType_t)(xTaskGetTickCount() - started) < pdMS_TO_TICKS(PF_HID_REBOOT_DELAY_MS));
    pf_system_reset();
}

static void usb_task(void *context)
{
    (void)context;
    TickType_t last_hid_report = xTaskGetTickCount();
    TickType_t last_storage_report = last_hid_report;
    TickType_t last_self_test_report = last_hid_report;

    TickType_t last_stack_sample = xTaskGetTickCount();

    while (true) {
        if ((TickType_t)(xTaskGetTickCount() - last_stack_sample) >= USB_STACK_SAMPLE_INTERVAL) {
            PF_LOGGING_INFO("stack", "USB minimum free stack: %u words",
                        (unsigned)uxTaskGetStackHighWaterMark(NULL));
            last_stack_sample = xTaskGetTickCount();
        }
        tud_task();
        if (pf_hid_take_reboot_request()) {
            reboot_after_usb_flush();
        }
        pf_usb_msc_task();
        pf_hid_poll_async_commands();
        cdc_cli_task();

        const TickType_t now = xTaskGetTickCount();
        if (tud_hid_ready() &&
            ((TickType_t)(now - last_hid_report) >= USB_HID_REPORT_INTERVAL)) {
            pf_hid_telemetry_report_t report;

            pf_hid_build_telemetry_report(&report, hid_sequence);
            if (tud_hid_report(PF_HID_TELEMETRY_REPORT_ID, &report, sizeof(report))) {
                ++hid_sequence;
                last_hid_report = now;
            }
        }

        if (tud_hid_ready() &&
            ((TickType_t)(now - last_storage_report) >= USB_HID_STORAGE_REPORT_INTERVAL)) {
            pf_hid_storage_report_t report;

            pf_hid_build_storage_report(&report, storage_sequence);
            if (tud_hid_report(PF_HID_STORAGE_REPORT_ID, &report, sizeof(report))) {
                ++storage_sequence;
                last_storage_report = now;
            }
        }

        if (tud_hid_ready() &&
            ((TickType_t)(now - last_self_test_report) >= USB_HID_SELF_TEST_INTERVAL)) {
            if (pf_hid_self_test_supported()) {
                pf_hid_self_test_report_t report;
                pf_hid_build_self_test_report(&report, self_test_sequence);
                if (tud_hid_report(PF_HID_SELF_TEST_REPORT_ID, &report, sizeof(report))) {
                    ++self_test_sequence;
                    last_self_test_report = now;
                }
            } else {
                last_self_test_report = now;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1U));
    }
}

void pf_usb_start(void)
{
    if (!pf_storage_worker_init(pf_cli_storage_execute)) {
        panic("Failed to create storage worker");
    }
    if (!tusb_init()) {
        panic("Failed to initialize TinyUSB");
    }
    /* ponytail: 250 ms boot detach permits stable enumeration after debugger/watchdog reset.
     * Boot uses busy_wait_ms before the scheduler; runtime MSC transitions use a
     * non-blocking deadline in pf_usb_msc_task. */
    tud_disconnect();
    /* The scheduler is not running yet; do not depend on alarm/RTOS wakeups here. */
    busy_wait_ms(USB_RECONNECT_DETACH_MS);
    const bool connected = tud_connect();
    if (!connected) {
        panic("Failed to connect USB");
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