#include "unity.h"
#include "cli.h"
#include "embedded_cli.h"

#include <string.h>

static char captured[4096];
static size_t captured_size;

static void capture_character(char character, void *context)
{
    TEST_ASSERT_EQUAL_PTR(captured, context);
    TEST_ASSERT_LESS_THAN(sizeof(captured) - 1u, captured_size);
    captured[captured_size++] = character;
    captured[captured_size] = '\0';
}

static void send_command(const char *command)
{
    while (*command != '\0') {
        size_t before = captured_size;
        pf_cli_receive_char(*command++);
        pf_cli_process();
        TEST_ASSERT_LESS_OR_EQUAL_UINT(1024u, captured_size - before);
    }
}

void setUp(void)
{
    captured_size = 0u;
    captured[0] = '\0';
    TEST_ASSERT_TRUE(pf_cli_init(capture_character, captured));
}

void tearDown(void) {}

void test_cli_emits_prompt_and_builtin_help(void)
{
    pf_cli_process();
    TEST_ASSERT_NOT_NULL(strstr(captured, "PicoForge> "));
    send_command("help\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "Show CLI version"));
    TEST_ASSERT_NOT_NULL(strstr(captured, "echo <text>"));
}

void test_cli_executes_version_and_echo(void)
{
    send_command("version\r\necho hello CDC\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "PicoForge CLI (Embedded CLI v0.1.4)"));
    TEST_ASSERT_NOT_NULL(strstr(captured, "hello CDC"));
}

void test_cli_rejects_version_arguments_and_unknown_commands(void)
{
    send_command("version extra\rmissing\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "Usage: version"));
    TEST_ASSERT_NOT_NULL(strstr(captured, "Unknown command"));
}

void test_cli_reset_discards_partial_input(void)
{
    send_command("vers");
    TEST_ASSERT_TRUE(pf_cli_init(capture_character, captured));
    send_command("echo fresh\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "fresh"));
    TEST_ASSERT_NULL(strstr(captured, "Unknown command"));
}

void test_cli_rejects_missing_output_callback(void)
{
    TEST_ASSERT_FALSE(pf_cli_init(NULL, NULL));
    pf_cli_receive_char('x');
    pf_cli_process();
    TEST_ASSERT_EQUAL_UINT(0u, captured_size);
}

void test_cli_supports_tab_completion_and_history(void)
{
    send_command("ver\t\r\x1b[A\r");
    const char *first = strstr(captured, "PicoForge CLI (Embedded CLI v0.1.4)");
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NOT_NULL(strstr(first + 1u, "PicoForge CLI (Embedded CLI v0.1.4)"));
    TEST_ASSERT_NULL(strstr(captured, "Unknown command"));
}

void test_cli_long_echo_fits_cdc_output_reserve(void)
{
    char command[128];
    memcpy(command, "echo ", 5u);
    memset(command + 5u, 'a', 120u);
    command[125] = '\r';
    command[126] = '\0';
    send_command(command);
    TEST_ASSERT_NULL(strstr(captured, "Unknown command"));
}

static void queued_command(EmbeddedCli *instance, char *args, void *context)
{
    (void)instance;
    TEST_ASSERT_EQUAL_STRING("fs", context);
    TEST_ASSERT_EQUAL_STRING("write /a \"hello world\"", args);
    pf_cli_print("Command queued.");
}

void test_cli_registers_async_command_and_preserves_raw_arguments(void)
{
    TEST_ASSERT_TRUE(pf_cli_add_binding((CliCommandBinding){
        "fs", "Filesystem commands", false, "fs", queued_command
    }));
    send_command("fs write /a \"hello world\"\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "Command queued."));
    pf_cli_print("FatFs: OK");
    TEST_ASSERT_NOT_NULL(strstr(captured, "FatFs: OK"));
}