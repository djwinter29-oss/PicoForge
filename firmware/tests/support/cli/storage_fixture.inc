// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

static char output[2048];
static FRESULT open_result;
static BYTE expected_mode;
static UINT written_size;
static FRESULT write_result;
static bool directory_entry;
static const pf_block_device_t device = {0};
static char cli_output[8192];
static size_t cli_output_size;
static bool worker_busy, msc_enabled, msc_ejected, msc_transition, msc_rejected, msc_request_ok;
static bool recovery_ok, benchmark_ok, benchmark_cancel_ok;
static unsigned msc_requests, recovery_requests, queued_commands;
static uint32_t benchmark_size;
static pf_file_benchmark_status_t benchmark_status;
static bool queue_ok;
static char queued_family[8], queued_arguments[PF_STORAGE_WORKER_ARGS_SIZE];

bool pf_storage_worker_busy(void) { return worker_busy; }
static bool mock_msc_enabled(int calls)
{
    (void)calls;
    return msc_enabled;
}
static bool mock_msc_ejected(int calls)
{
    (void)calls;
    return msc_ejected;
}
static bool mock_msc_transition(int calls)
{
    (void)calls;
    return msc_transition;
}
static bool mock_msc_rejected(int calls)
{
    (void)calls;
    return msc_rejected;
}
static bool mock_msc_request(bool enabled, int calls)
{
    (void)calls;
    ++msc_requests;
    if (msc_request_ok) msc_enabled = enabled;
    return msc_request_ok;
}
static bool mock_msc_recovery(int calls)
{
    (void)calls;
    ++recovery_requests;
    return recovery_ok;
}
bool pf_storage_worker_benchmark_start(uint32_t size_mib, bool host_owned)
{
    TEST_ASSERT_FALSE(host_owned);
    benchmark_size = size_mib;
    return benchmark_ok;
}
bool pf_storage_worker_benchmark_cancel(void) { return benchmark_cancel_ok; }
void pf_storage_worker_benchmark_status(pf_file_benchmark_status_t *status) { *status = benchmark_status; }

static void capture_cli(char character, void *context)
{
    (void)context;
    TEST_ASSERT_LESS_THAN(sizeof(cli_output) - 1u, cli_output_size);
    cli_output[cli_output_size++] = character;
    cli_output[cli_output_size] = '\0';
}

static bool queue_command(const char *family, const char *arguments, void *context)
{
    (void)context;
    ++queued_commands;
    (void)snprintf(queued_family, sizeof(queued_family), "%s", family);
    (void)snprintf(queued_arguments, sizeof(queued_arguments), "%s", arguments);
    return queue_ok;
}

static void send_cli(const char *command)
{
    cli_output_size = 0u;
    cli_output[0] = '\0';
    while (*command != '\0') {
        pf_cli_receive_char(*command++);
        pf_cli_process();
    }
}

static FRESULT open_file(FIL *file, const TCHAR *path, BYTE mode, int calls)
{
    (void)calls;
    TEST_ASSERT_EQUAL_STRING("0:/a", path);
    TEST_ASSERT_EQUAL_HEX8(expected_mode, mode);
    memset(file, 0, sizeof(*file));
    file->obj.objsize = 100u;
    return open_result;
}

static FRESULT write_file(FIL *file, const void *buffer, UINT size, UINT *written, int calls)
{
    (void)file;
    (void)calls;
    TEST_ASSERT_EQUAL_STRING("hello world", buffer);
    TEST_ASSERT_EQUAL_UINT(11u, size);
    *written = written_size;
    return write_result;
}

static FRESULT read_file(FIL *file, void *buffer, UINT size, UINT *received, int calls)
{
    (void)file;
    (void)calls;
    const unsigned char text[] = { 'a', '\n', 0x1bu, 'z' };
    TEST_ASSERT_EQUAL_UINT(4u, size);
    memcpy(buffer, text, sizeof(text));
    *received = sizeof(text);
    return FR_OK;
}

static FRESULT read_directory(DIR *directory, FILINFO *info, int calls)
{
    (void)directory;
    memset(info, 0, sizeof(*info));
    if (calls == 0) {
        memcpy(info->fname, "folder", 7u);
        info->fattrib = AM_DIR;
        info->fsize = 10u;
    }

    return FR_OK;
}

static FRESULT read_long_directory_name(DIR *directory, FILINFO *info, int calls)
{
    (void)directory;
    memset(info, 0, sizeof(*info));
    if (calls == 0) {
        memset(info->fname, 'a', 199u);
        memcpy(info->fname + 199u, "\xe2\x82\xac" "tail", 8u);
    }
    return FR_OK;
}

static FRESULT read_exact_directory_name(DIR *directory, FILINFO *info, int calls)
{
    (void)directory;
    memset(info, 0, sizeof(*info));
    if (calls == 0) memset(info->fname, 'a', 200u);
    return FR_OK;
}

static FRESULT read_unicode_directory_name(DIR *directory, FILINFO *info, int calls)
{
    (void)directory;
    memset(info, 0, sizeof(*info));
    if (calls == 0) memcpy(info->fname, "\xe2\x82\xac.txt", 8u);
    return FR_OK;
}

static FRESULT read_multiple_unsupported_names(DIR *directory, FILINFO *info, int calls)
{
    (void)directory;
    memset(info, 0, sizeof(*info));
    if (calls < 2) {
        const char *name = calls == 0 ? "\xe2\x82\xac.txt" : "another-name";
        memcpy(info->fname, name, strlen(name) + 1u);
    }
    return FR_OK;
}

static FRESULT file_stat(const TCHAR *path, FILINFO *info, int calls)
{
    (void)calls;
    TEST_ASSERT_EQUAL_STRING("0:/a", path);
    memset(info, 0, sizeof(*info));
    info->fattrib = directory_entry ? AM_DIR : AM_ARC;
    info->fsize = 42u;
    return FR_OK;
}

static FRESULT rename_file(const TCHAR *source, const TCHAR *target, int calls)
{
    (void)calls;
    TEST_ASSERT_EQUAL_STRING("0:/a", source);
    TEST_ASSERT_EQUAL_STRING("0:/b", target);
    return FR_EXIST;
}

static bool reply(const char *line, void *context)
{
    (void)context;
    TEST_ASSERT_LESS_THAN(sizeof(output), strlen(output) + strlen(line) + 2u);
    strcat(output, line);
    strcat(output, "\n");
    return true;
}

static unsigned occurrences(const char *text, const char *needle)
{
    unsigned count = 0u;
    const size_t size = strlen(needle);
    while ((text = strstr(text, needle)) != NULL) {
        ++count;
        text += size;
    }
    return count;
}

static void run(const char *arguments)
{
    char tokens[PF_STORAGE_WORKER_ARGS_SIZE] = { 0 };
    size_t size = strlen(arguments);
    TEST_ASSERT_LESS_THAN(sizeof(tokens), size);
    memcpy(tokens, arguments, size + 1u);
    embeddedCliTokenizeArgs(tokens);
    uint16_t count = embeddedCliGetTokenCount(tokens);
    const char *operation = count > 0u ? embeddedCliGetToken(tokens, 1u) : NULL;
    const char *first = count > 1u ? embeddedCliGetToken(tokens, 2u) : NULL;
    const char *second = count > 2u ? embeddedCliGetToken(tokens, 3u) : NULL;
    pf_cli_fs_execute(operation, count, first, second, reply, NULL);
}

static FRESULT mkdir_denied(const TCHAR *path, int calls)
{
    (void)calls;
    TEST_ASSERT_EQUAL_STRING("0:/new", path);
    return FR_DENIED;
}

static void storage_set_up(void)
{
    output[0] = '\0';
    cli_output[0] = '\0';
    cli_output_size = 0u;
    open_result = FR_OK;
    expected_mode = FA_READ;
    written_size = 11u;
    write_result = FR_OK;
    directory_entry = false;
    worker_busy = msc_enabled = msc_ejected = msc_transition = msc_rejected = false;
    msc_request_ok = recovery_ok = benchmark_ok = benchmark_cancel_ok = queue_ok = true;
    msc_requests = recovery_requests = queued_commands = 0u;
    benchmark_size = 0u;
    memset(&benchmark_status, 0, sizeof(benchmark_status));
    queued_family[0] = queued_arguments[0] = '\0';
    Mockusb_msc_Init();
    pf_usb_msc_is_enabled_StubWithCallback(mock_msc_enabled);
    pf_usb_msc_is_ejected_StubWithCallback(mock_msc_ejected);
    pf_usb_msc_transition_pending_StubWithCallback(mock_msc_transition);
    pf_usb_msc_request_rejected_StubWithCallback(mock_msc_rejected);
    pf_usb_msc_request_StubWithCallback(mock_msc_request);
    pf_usb_msc_request_recovery_StubWithCallback(mock_msc_recovery);
    TEST_ASSERT_TRUE(pf_cli_init(capture_cli, NULL));
    TEST_ASSERT_TRUE(pf_cli_storage_register(queue_command, NULL));
    TEST_ASSERT_TRUE(pf_cli_fs_register(queue_command, NULL));
}
static void storage_tear_down(void)
{
    Mockusb_msc_Verify();
    Mockusb_msc_Destroy();
}

static bool disconnected_reply(const char *line, void *context)
{
    (void)line;
    (void)context;
    return false;
}

static bool interrupt_first_reply(const char *line, void *context)
{
    unsigned *calls = context;
    if ((*calls)++ == 0u) return false;
    return reply(line, NULL);
}

static FRESULT read_full_chunk(FIL *file, void *buffer, UINT size, UINT *received, int calls)
{
    (void)file;
    TEST_ASSERT_EQUAL_UINT(48u, size);
    memset(buffer, 0x01, size);
    *received = size;
    return calls == 0 ? FR_OK : FR_DISK_ERR;
}

static FRESULT open_directory_root(DIR *directory, const TCHAR *path, int calls)
{
    (void)directory;
    (void)calls;
    TEST_ASSERT_EQUAL_STRING("0:/", path);
    return FR_OK;
}
