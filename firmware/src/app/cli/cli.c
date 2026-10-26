/**
 * @file
 * @brief Product CLI instance, prompt, built-in commands and static configuration.
 */
#include "cli.h"

#include "embedded_cli.h"

#include <stddef.h>

/** Static CLI allocation in bytes; embeddedCliNew() fails, and init returns false, if the configuration needs more. */
#define CLI_STORAGE_SIZE 4096u
/** Input ring buffer size in bytes. */
#define CLI_RX_BUFFER_SIZE 64u
/** Longest command line in bytes. */
#define CLI_COMMAND_BUFFER_SIZE 128u
/** Command history size in bytes. */
#define CLI_HISTORY_BUFFER_SIZE 256u
/** Binding slots besides the library help command; the built-in version and echo commands use two. */
#define CLI_MAX_BINDINGS 8u

static CLI_UINT cli_storage[BYTES_TO_CLI_UINTS(CLI_STORAGE_SIZE)]; /**< Application-lifetime CLI allocation. */
static EmbeddedCli *cli; /**< Active singleton using cli_storage. */
static pf_cli_write_char_t output; /**< Borrowed synchronous session output callback. */
static void *output_context; /**< Borrowed callback context, valid for the session. */

/** @brief Embedded CLI output hook forwarding to the session callback. */
static void write_character(EmbeddedCli *instance, char character)
{
    (void)instance;
    output(character, output_context); /* Only installed after pf_cli_init() verified a non-NULL callback. */
}

/** @brief `version`: print the CLI banner; takes no arguments. */
static void version_command(EmbeddedCli *instance, char *args, void *context)
{
    (void)context;
    if (embeddedCliGetTokenCount(args) != 0u) {
        embeddedCliPrint(instance, "Usage: version");
        return;
    }
    embeddedCliPrint(instance, "PicoForge CLI (Embedded CLI)");
}

/** @brief `echo <text>`: print the arguments; bounded by CLI_COMMAND_BUFFER_SIZE. */
static void echo_command(EmbeddedCli *instance, char *args, void *context)
{
    (void)context;
    embeddedCliPrint(instance, args != NULL ? args : "");
}

bool pf_cli_init(pf_cli_write_char_t write_char, void *context)
{
    EmbeddedCliConfig *config;

    cli = NULL;
    if (write_char == NULL) {
        return false;
    }
    output = write_char;
    output_context = context;
    config = embeddedCliDefaultConfig();
    config->invitation = "PicoForge> ";
    config->rxBufferSize = CLI_RX_BUFFER_SIZE;
    config->cmdBufferSize = CLI_COMMAND_BUFFER_SIZE;
    config->historyBufferSize = CLI_HISTORY_BUFFER_SIZE;
    config->maxBindingCount = CLI_MAX_BINDINGS;
    config->enableAutoComplete = false;
    config->cliBuffer = cli_storage;
    config->cliBufferSize = sizeof(cli_storage);
    cli = embeddedCliNew(config);
    if (cli == NULL) {
        return false;
    }
    cli->writeChar = write_character;
    return embeddedCliAddBinding(cli, (CliCommandBinding){
               "version", "Show CLI version", true, NULL, version_command
           }) &&
           embeddedCliAddBinding(cli, (CliCommandBinding){
               "echo", "echo <text>: print text", false, NULL, echo_command
           });
}

void pf_cli_receive_char(char character)
{
    if (cli != NULL) {
        embeddedCliReceiveChar(cli, character);
    }
}

void pf_cli_process(void)
{
    if (cli != NULL) {
        embeddedCliProcess(cli);
    }
}

bool pf_cli_add_binding(CliCommandBinding binding)
{
    return (cli != NULL) && embeddedCliAddBinding(cli, binding);
}

void pf_cli_print(const char *line)
{
    if ((cli != NULL) && (line != NULL)) {
        embeddedCliPrint(cli, line);
    }
}