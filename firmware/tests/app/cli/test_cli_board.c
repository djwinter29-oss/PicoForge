#include "unity.h"
TEST_SOURCE_FILE("embedded_cli_adapter.c")
#include "cli.h"
#include "app/cli/cli_board.h"
#include "app/cli/cli_config.h"
#include "app/cli/cli_fs.h"
#include "app/cli/cli_storage.h"
#include "app/cli/cli_internal.h"
#include "embedded_cli.h"
#include "Mockboot_config.h"
#include "Mockusb_msc.h"
#include "Mockwireless.h"
#include <string.h>

#include "cli/device_fixture.inc"

void setUp(void) { device_set_up(); }
void tearDown(void) { device_tear_down(); }

void test_board_reads_actual_clock_led_uptime_and_build_version(void)
{
    send("board get clock\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "clock=125000000 Hz"));
    send("board get version\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "version=1.2.3"));
    send("board get uptime\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "uptime=123 ms"));
    send("board set led on\r");
    TEST_ASSERT_TRUE(led_on);
    send("board get led\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "led=on"));
    send("board set led off\r");
    TEST_ASSERT_FALSE(led_on);
}

void test_board_invalid_requests_have_no_side_effects(void)
{
    send("board set led on extra\r");
    send("board set led maybe\r");
    send("board reboot extra\r");
    send("board\r");
    TEST_ASSERT_EQUAL_UINT(0u, led_changes);
    TEST_ASSERT_EQUAL_UINT(0u, reboots);
    send("board get led\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "led=off"));
    send("board set led on\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "OK LED on"));
}

void test_radio_status_uses_sanitized_state_and_never_prints_ssid_or_secrets(void)
{
    radio_report[4] = 2u;
    radio_report[5] = 192u;
    radio_report[6] = 168u;
    radio_report[7] = 66u;
    radio_report[8] = 87u;
    memcpy(radio_report + 9u, "\x1b[2Jprivate-ssid", 16u);
    radio_report[42] = radio_report[43] = radio_report[46] = 1u;
    radio_report[45] = 2u;
    send("radio status\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "wifi=connected ip=192.168.66.87\r\nbluetooth=connected\r\n"));
    TEST_ASSERT_NULL(strstr(captured, "configured"));
    TEST_ASSERT_NULL(strstr(captured, "private-ssid"));
    send("radio missing\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "ERR radio status"));
}

void test_oversized_board_command_does_not_dispatch_its_valid_prefix(void)
{
    send("board reboot");
    for (unsigned i = 0u; i < 114u; ++i) send(" ");
    send("EXTRA\r");
    TEST_ASSERT_EQUAL_UINT(0u, reboots);
    TEST_ASSERT_NOT_NULL(strstr(captured, "ERR input overflow"));
}
