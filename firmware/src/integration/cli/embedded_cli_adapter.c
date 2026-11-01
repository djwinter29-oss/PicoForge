/**
 * @file embedded_cli_adapter.c
 * @brief Pinned Embedded CLI adapter rejecting editor/RX overflow before command dispatch.
 *
 * The upstream public API has no editor length or rejection hook. Include its implementation here to reuse the
 * editor and parser without modifying the submodule; only the input pump below replaces upstream behavior.
 */
#define embeddedCliProcess embeddedCliProcessUnchecked
#include "third_party/embedded-cli/lib/src/embedded_cli.c"
#undef embeddedCliProcess

/** Rejection persists through edits/history until the next line ending; upstream uses flag bits 0..5. */
#define PF_CLI_FLAG_REJECT_LINE 0x40u

/** @brief Consume input with upstream editing semantics, rejecting any command whose bytes were lost. */
void embeddedCliProcess(EmbeddedCli *cli)
{
    if (cli->writeChar == NULL) return;
    PREPARE_IMPL(cli);
    if (!IS_FLAG_SET(impl->flags, CLI_FLAG_INIT_COMPLETE)) {
        SET_FLAG(impl->flags, CLI_FLAG_INIT_COMPLETE);
        writeToOutput(cli, impl->invitation);
    }
    if (IS_FLAG_SET(impl->flags, CLI_FLAG_OVERFLOW)) {
        SET_FLAG(impl->flags, PF_CLI_FLAG_REJECT_LINE);
        UNSET_U8FLAG(impl->flags, CLI_FLAG_OVERFLOW);
        /* Queued line endings cannot locate lost bytes; discard the backlog and wait for a fresh line ending. */
        impl->rxBuffer.front = impl->rxBuffer.back;
    }
    while (fifoBufAvailable(&impl->rxBuffer)) {
        char c = fifoBufPop(&impl->rxBuffer);
        if (!IS_FLAG_SET(impl->flags, CLI_FLAG_ESCAPE_MODE) && !(impl->lastChar == 0x1B && c == '[') &&
            isDisplayableChar(c) &&
            impl->cmdSize + 2 >= impl->cmdMaxSize) {
            SET_FLAG(impl->flags, PF_CLI_FLAG_REJECT_LINE);
        }
        if (IS_FLAG_SET(impl->flags, PF_CLI_FLAG_REJECT_LINE)) {
            if (c == '\r' || c == '\n') {
                clearCurrentLine(cli);
                impl->cmdSize = 0;
                impl->cmdBuffer[0] = '\0';
                impl->inputLineLength = 0;
                impl->cursorPos = 0;
                impl->history.current = 0;
                UNSET_U8FLAG(impl->flags, PF_CLI_FLAG_REJECT_LINE | CLI_FLAG_ESCAPE_MODE);
                writeToOutput(cli, "ERR input overflow; command rejected. Re-enter the complete command.\r\n");
                writeToOutput(cli, impl->invitation);
            }
        } else {
            if (IS_FLAG_SET(impl->flags, CLI_FLAG_ESCAPE_MODE)) {
                onEscapedInput(cli, c);
            } else if (impl->lastChar == 0x1B && c == '[') {
                SET_FLAG(impl->flags, CLI_FLAG_ESCAPE_MODE);
            } else if (isControlChar(c)) {
                onControlInput(cli, c);
            } else if (isDisplayableChar(c)) {
                onCharInput(cli, c);
            }
            printLiveAutocompletion(cli);
        }
        impl->lastChar = c;
    }
}
