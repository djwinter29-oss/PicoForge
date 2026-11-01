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

void test_help_with_every_production_binding_fits_output_reserve(void)
{
    send("help\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, " * config\r\n\tget [clock | storage | wifi | bluetooth]"));
    TEST_ASSERT_NOT_NULL(strstr(captured, " * storage\r\n\tget [status | usb | benchmark]\r\n\tset usb <on | off>"));
    TEST_ASSERT_NULL(strstr(captured, " * msc"));
    TEST_ASSERT_NULL(strstr(captured, " * benchmark"));
    TEST_ASSERT_NOT_NULL(strstr(captured, "\tformat --confirm (destroys files)\r\n\tmount (maintenance)"));
    TEST_ASSERT_NOT_NULL(strstr(captured, " * fs\r\n\tls [path]"));
    TEST_ASSERT_LESS_OR_EQUAL_UINT(1024u, largest_response);
    send("help config\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "key=value"));
    TEST_ASSERT_NOT_NULL(strstr(captured, "(saves and reboots)"));
    send("board --help\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "\tget <clock | led | version | uptime>\r\n\tset led <on | off>\r\n\treboot"));
    TEST_ASSERT_EQUAL_UINT(0u, reboots);
}

void test_group_registration_rejects_missing_reset_scheduler(void)
{
    TEST_ASSERT_FALSE(pf_cli_board_register(NULL));
    TEST_ASSERT_FALSE(pf_cli_config_register(NULL));
    TEST_ASSERT_FALSE(pf_cli_storage_register(NULL, NULL));
    TEST_ASSERT_FALSE(pf_cli_fs_register(NULL, NULL));
}
