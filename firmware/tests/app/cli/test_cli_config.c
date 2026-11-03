#include "unity.h"
TEST_SOURCE_FILE("embedded_cli_adapter.c")
#include "cli.h"
#include "app/cli/cli_board.h"
#include "app/cli/cli_config.h"
#include "app/cli/cli_fs.h"
#include "app/cli/cli_storage.h"
#include "app/cli/cli_fatfs_result.h"
#include "app/cli/cli_internal.h"
#include "embedded_cli.h"
#include "Mockboot_config.h"
#include "Mockusb_msc.h"
#include "Mockwireless.h"
#include <string.h>

#include "cli/device_fixture.inc"

void setUp(void) { device_set_up(); }
void tearDown(void) { device_tear_down(); }

void test_config_reads_saved_targets_not_running_clock(void)
{
    clock_profile = PF_SYSCLK_250MHZ;
    send("config get\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "clock=250 MHz (next boot)"));
    TEST_ASSERT_NOT_NULL(strstr(captured, "storage=default"));
    TEST_ASSERT_NOT_NULL(strstr(captured, "bluetooth=enabled"));
    TEST_ASSERT_NULL(strstr(captured, "125000000"));
    send("config get missing\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "ERR config get"));
    TEST_ASSERT_EQUAL_UINT(0u, setters);
}

void test_config_validates_all_assignments_then_saves_and_requests_reboot(void)
{
    send("config set clock = 200, storage=high, wifi=disabled, bluetooth=enabled\r");
    TEST_ASSERT_EQUAL_UINT(PF_SYSCLK_200MHZ, saved_clock);
    TEST_ASSERT_EQUAL_UINT(PF_STORAGE_SPEED_HIGH, saved_speed);
    TEST_ASSERT_EQUAL_UINT(PF_WIRELESS_BLE, saved_radios);
    TEST_ASSERT_EQUAL_UINT(1u, saves);
    TEST_ASSERT_EQUAL_UINT(1u, reboots);
    TEST_ASSERT_NOT_NULL(strstr(captured, "OK config saved"));
    TEST_ASSERT_NOT_NULL(strstr(captured, "QUEUED reboot"));
}

void test_config_rejects_unknown_duplicate_empty_and_out_of_range_assignments_before_staging(void)
{
    static const char *const requests[] = {
        "config set clock=200,unknown=x\r", "config set clock=150,clock=200\r",
        "config set storage=high,\r", "config set clock=125\r", "config set clock=+150\r",
        "config set storage=50\r", "config set wifi=maybe\r", "config set clock=\r",
        "config set password=secret\r", "config set =150\r", "config set\r",
        "config set clock=150,,storage=high\r"
    };
    for (size_t i = 0u; i < sizeof(requests) / sizeof(requests[0]); ++i) {
        send(requests[i]);
        TEST_ASSERT_NOT_NULL(strstr(captured, "ERR"));
    }
    TEST_ASSERT_EQUAL_UINT(0u, setters);
    TEST_ASSERT_EQUAL_UINT(0u, saves);
    TEST_ASSERT_EQUAL_UINT(0u, reboots);
}

void test_config_stage_and_save_failures_discard_targets_without_reboot(void)
{
    stage_ok = false;
    send("config set clock=200\r");
    TEST_ASSERT_EQUAL_UINT(1u, discards);
    TEST_ASSERT_EQUAL_UINT(0u, saves);
    stage_ok = true;
    save_ok = false;
    send("config set clock=250,storage=high\r");
    TEST_ASSERT_EQUAL_UINT(2u, discards);
    TEST_ASSERT_EQUAL_UINT(PF_SYSCLK_DEFAULT, clock_profile);
    TEST_ASSERT_EQUAL_UINT(PF_STORAGE_SPEED_DEFAULT, speed_profile);
    TEST_ASSERT_EQUAL_UINT(0u, reboots);
    TEST_ASSERT_NOT_NULL(strstr(captured, "WARN Save unconfirmed"));
}

void test_config_partial_staging_failure_restores_every_target(void)
{
    for (unsigned failure = 2u; failure <= 3u; ++failure) {
        setters = 0u;
        stage_failure_at = failure;
        send("config set clock=250,storage=high,wifi=disabled\r");
        TEST_ASSERT_EQUAL_UINT(PF_SYSCLK_DEFAULT, clock_profile);
        TEST_ASSERT_EQUAL_UINT(PF_STORAGE_SPEED_DEFAULT, speed_profile);
        TEST_ASSERT_EQUAL_UINT(PF_WIRELESS_WIFI | PF_WIRELESS_BLE, radio_flags);
    }
    TEST_ASSERT_EQUAL_UINT(2u, discards);
    TEST_ASSERT_EQUAL_UINT(0u, saves);
    TEST_ASSERT_EQUAL_UINT(0u, reboots);
}

void test_config_rejects_assignment_buffer_limit_without_applying_a_truncated_request(void)
{
    char command[128] = "config set clock=150";
    const size_t length = strlen(command);
    memset(command + length, ' ', 111u - length);
    command[111] = '\r';
    command[112] = '\0';
    send(command);
    TEST_ASSERT_NOT_NULL(strstr(captured, "ERR configuration assignments"));
    TEST_ASSERT_EQUAL_UINT(0u, setters);
    TEST_ASSERT_EQUAL_UINT(0u, reboots);
}

void test_config_and_reboot_refuse_busy_usb_ownership_and_transitions(void)
{
    for (unsigned i = 0u; i < 3u; ++i) {
        worker_busy = i == 0u;
        host_owned = i == 1u;
        transition = i == 2u;
        send("config set clock=200\r");
        TEST_ASSERT_NOT_NULL(strstr(captured, "ERR storage busy"));
        send("board reboot\r");
        TEST_ASSERT_NOT_NULL(strstr(captured, "ERR storage busy"));
    }
    TEST_ASSERT_EQUAL_UINT(0u, setters);
    TEST_ASSERT_EQUAL_UINT(0u, reboots);
    worker_busy = host_owned = transition = false;
    send("board reboot\r");
    TEST_ASSERT_EQUAL_UINT(1u, reboots);
}

void test_config_set_accepts_tab_and_mixed_assignment_whitespace(void)
{
    char arguments[] = "set\t \tclock = 200 , storage = high";
    /* Tab is an editor completion key; exercise raw binding input independently of the terminal editor. */
    pf_cli_config_command(NULL, arguments, NULL);
    TEST_ASSERT_EQUAL_UINT(PF_SYSCLK_200MHZ, clock_profile);
    TEST_ASSERT_EQUAL_UINT(PF_STORAGE_SPEED_HIGH, speed_profile);
    TEST_ASSERT_EQUAL_UINT(1u, saves);
    TEST_ASSERT_EQUAL_UINT(1u, reboots);
}
