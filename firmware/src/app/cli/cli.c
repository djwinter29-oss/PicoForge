/** @file
 * @brief Product CLI instance, prompt, built-in commands and static configuration.
 */
#include "cli.h"

#include "embedded_cli.h"

#include <stddef.h>

#define CLI_STORAGE_SIZE 4096u

static CLI_UINT cli_storage[BYTES_TO_CLI_UINTS(CLI_STORAGE_SIZE)]; /**< Application-lifetime CLI allocation. */
static EmbeddedCli *cli; /**< Active singleton using cli_storage. */
static pf_cli_write_char_t output; /**< Borrowed synchronous session output callback. */
static void *output_context; /**< Borrowed callback context, valid for the session. */

static void write_character(EmbeddedCli *instance, char character)
{
    (void)instance;
    output(character, output_context);
}

static void version_command(EmbeddedCli *instance, char *args, void *context)
{
    (void)context;
    if (embeddedCliGetTokenCount(args) != 0u) {
        embeddedCliPrint(instance, "Usage: version");
        return;
    }
    embeddedCliPrint(instance, "PicoForge CLI (Embedded CLI v0.1.4)");
}

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
    config->rxBufferSize = 64u;
    config->cmdBufferSize = 128u;
    config->historyBufferSize = 256u;
    config->maxBindingCount = 8u;
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