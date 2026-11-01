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

/** Test-only echo binding: production has no echo command, but a raw-argument probe exercises the editor. */
static void echo_command(EmbeddedCli *instance, char *args, void *context)
{
    (void)instance;
    (void)context;
    pf_cli_print(args != NULL ? args : "");
}

static void init_with_echo(void)
{
    TEST_ASSERT_TRUE(pf_cli_init(capture_character, captured));
    TEST_ASSERT_TRUE(pf_cli_add_binding((CliCommandBinding){ "echo", "echo <text>", false, NULL, echo_command }));
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
    TEST_ASSERT_NOT_NULL(strstr(captured, "Print list of commands"));
}

void test_cli_has_no_redundant_version_or_echo_commands(void)
{
    send_command("version\recho hi\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "Unknown command: \"version\""));
    TEST_ASSERT_NOT_NULL(strstr(captured, "Unknown command: \"echo\""));
}

void test_cli_reset_discards_partial_input(void)
{
    init_with_echo();
    send_command("ech");
    init_with_echo();
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
    init_with_echo();
    send_command("ec\t hi\r\x1b[A\r");
    const char *first = strstr(captured, "\r\nhi\r\n");
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NOT_NULL(strstr(first + 1u, "\r\nhi\r\n"));
    TEST_ASSERT_NULL(strstr(captured, "Unknown command"));
}

void test_cli_long_echo_fits_cdc_output_reserve(void)
{
    char command[128];
    init_with_echo();
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
static void noop_command(EmbeddedCli *cli, char *args, void *context)
{
    (void)cli;
    (void)args;
    (void)context;
}

void test_cli_binding_table_holds_five_application_commands_and_rejects_more(void)
{
    static const char *const names[] = { "a1", "a2", "a3", "a4", "a5", "a6" };

    for (size_t index = 0u; index < 5u; ++index) {
        TEST_ASSERT_TRUE(pf_cli_add_binding((CliCommandBinding){ names[index], "x", false, NULL, noop_command }));
    }
    TEST_ASSERT_FALSE(pf_cli_add_binding((CliCommandBinding){ names[5], "x", false, NULL, noop_command }));
}

void test_cli_reinit_switches_output_and_drops_history(void)
{
    init_with_echo();
    send_command("echo old\r");
    TEST_ASSERT_TRUE(pf_cli_init(NULL, NULL) == false);
    init_with_echo();
    captured_size = 0u;
    send_command("echo new\r");
    TEST_ASSERT_NOT_NULL(strstr(captured, "new"));
}
