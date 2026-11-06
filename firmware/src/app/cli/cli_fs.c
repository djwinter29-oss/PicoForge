// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

/**
 * @file
 * @brief Filesystem command registration, validation and execution.
 */
#include "cli_fs.h"
#include "cli.h"
#include "cli_fatfs_result.h"
#include "cli_internal.h"
#include "platform/storage/fatfs_volume.h"
#include "platform/storage/storage.h"
#include "embedded_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    pf_cli_worker_submit_fn_t submit; /**< Borrowed worker submission callback. */
    void *context; /**< Borrowed callback context. */
} fs_dispatch;

/** Longest accepted absolute path in bytes, excluding the terminator. */
#define PF_CLI_PATH_MAX 100u
/** Buffer for a path with the FatFs volume prefix and terminator. */
#define PF_CLI_VOLUME_PATH_SIZE (PF_CLI_PATH_MAX + 4u)
/** Bytes shown by `fs cat` when no limit is given. */
#define PF_CLI_CAT_DEFAULT 1024u
/** Largest accepted `fs cat` limit in bytes. */
#define PF_CLI_CAT_MAX 4096u
/** Bytes read per `fs cat` reply line; each byte may expand to a four-character \xNN escape. */
#define PF_CLI_CAT_CHUNK 48u

/** Element count of a true array. */
#define PF_CLI_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

_Static_assert(PF_CLI_CAT_CHUNK * 4u < PF_STORAGE_WORKER_REPLY_SIZE, "cat escapes must fit one reply line");
_Static_assert(PF_CLI_VOLUME_PATH_SIZE >= sizeof(PF_FATFS_VOLUME_PATH) + PF_CLI_PATH_MAX, "volume path buffer");

/** @brief True for one nonempty printable-ASCII path component supported by FatFs and this CLI. */
static bool valid_path_component(const char *component, size_t size)
{
    if (component == NULL || size == 0u ||
        (size == 1u && component[0] == '.') ||
        (size == 2u && component[0] == '.' && component[1] == '.')) {
        return false;
    }
    for (size_t index = 0u; index < size; ++index) {
        unsigned char character = (unsigned char)component[index];
        if (character < 32u || character > 126u || strchr(":\\*?\"<>|", character) != NULL) {
            return false;
        }
    }
    return true;
}

/** @brief True for an absolute printable-ASCII path without FAT-reserved characters or . and .. segments. */
static bool valid_path(const char *path)
{
    if (path == NULL || path[0] != '/' || strlen(path) > PF_CLI_PATH_MAX) return false;
    const char *segment = path + 1u;
    for (const char *position = segment; ; ++position) {
        if (*position == '/' || *position == '\0') {
            size_t size = (size_t)(position - segment);
            if (size != 0u && !valid_path_component(segment, size)) return false;
            if (*position == '\0') return true;
            segment = position + 1u;
        }
    }
}

/** @brief Prefix @p path with the FatFs volume; @p path was validated to fit. */
static void volume_path(const char *path, char target[PF_CLI_VOLUME_PATH_SIZE])
{
    (void)snprintf(target, PF_CLI_VOLUME_PATH_SIZE, "%s%s", PF_FATFS_VOLUME_PATH, path);
}

/**
 * @brief List one directory, one entry per reply line, with one path-limit summary and final FatFs result.
 * @note Names are UTF-8 (FF_LFN_UNICODE 2), so bytes of 128 and above pass through; only control characters
 *       are replaced.
 */
static void list_command(const char *path, pf_storage_worker_reply_fn_t reply, void *context)
{
    DIR directory;
    FILINFO info;
    char line[PF_STORAGE_WORKER_REPLY_SIZE];
    bool interrupted = false;
    bool unsupported_path_found = false;
    const char *parent_path = path + strlen(PF_FATFS_VOLUME_PATH);
    const size_t parent_size = strlen(parent_path);
    const bool parent_supported = valid_path(parent_path);
    const bool parent_has_trailing_slash = parent_path[parent_size - 1u] == '/';
    FRESULT result = f_opendir(&directory, path);

    if (result != FR_OK) {
        pf_cli_report_result(result, reply, context);
        return;
    }
    for (;;) {
        result = f_readdir(&directory, &info);
        if ((result != FR_OK) || (info.fname[0] == '\0')) {
            break;
        }
        size_t name_size = strlen(info.fname);
        bool shortened = name_size > 200u;
        if (shortened) {
            name_size = 200u;
            while (name_size > 0u && ((unsigned char)info.fname[name_size] & 0xc0u) == 0x80u) --name_size;
        }
        size_t filename_size = strlen(info.fname);
        bool cli_path_supported = parent_supported && valid_path_component(info.fname, filename_size) &&
            parent_size + (parent_has_trailing_slash ? 0u : 1u) + filename_size <= PF_CLI_PATH_MAX;
        unsupported_path_found |= !cli_path_supported;
        (void)snprintf(line, sizeof(line), "%c %llu %.*s%s",
                       (info.fattrib & AM_DIR) != 0u ? 'd' : '-',
                       (unsigned long long)info.fsize, (int)name_size, info.fname,
                       shortened ? " [name truncated]" : "");
        for (char *position = line; *position != '\0'; ++position) {
            if (((unsigned char)*position < 32u) || ((unsigned char)*position == 127u)) {
                *position = '?';
            }
        }
        if (!reply(line, context)) {
            interrupted = true;
            break;
        }
    }
    FRESULT close_result = f_closedir(&directory);
    if (unsupported_path_found) {
        (void)reply("WARN Some listed paths exceed CLI ASCII/100-byte limits; use host file tools for those entries.", context);
    }
    if (interrupted) (void)reply("WARN Listing output interrupted; FatFs status does not confirm complete delivery.", context);
    pf_cli_report_result(result != FR_OK ? result : close_result, reply, context);
}

/** @brief Report size, attributes and timestamp of one path. */
static void stat_command(const char *path, pf_storage_worker_reply_fn_t reply, void *context)
{
    FILINFO info;
    char line[PF_STORAGE_WORKER_REPLY_SIZE];
    FRESULT result = f_stat(path, &info);

    if (result == FR_OK) {
        (void)snprintf(line, sizeof(line),
                       "size=%llu attributes=0x%02x date=%04u-%02u-%02u time=%02u:%02u:%02u",
                       (unsigned long long)info.fsize, (unsigned)info.fattrib,
                       1980u + (info.fdate >> 9u), (info.fdate >> 5u) & 15u, info.fdate & 31u,
                       info.ftime >> 11u, (info.ftime >> 5u) & 63u, (info.ftime & 31u) * 2u);
        (void)reply(line, context);
    } else {
        pf_cli_report_result(result, reply, context);
    }
}

/** @brief Show up to @p limit bytes of a file with non-printable bytes escaped as \xNN; always closes the file. */
static void cat_command(const char *path, unsigned long limit,
                        pf_storage_worker_reply_fn_t reply, void *context)
{
    FIL file;
    unsigned char bytes[PF_CLI_CAT_CHUNK];
    char line[PF_STORAGE_WORKER_REPLY_SIZE];
    FRESULT result = f_open(&file, path, FA_READ);
    unsigned long total = 0u;
    bool interrupted = false;

    if (result != FR_OK) {
        pf_cli_report_result(result, reply, context);
        return;
    }
    while (total < limit) {
        UINT received = 0u;
        UINT size = limit - total < sizeof(bytes) ? (UINT)(limit - total) : sizeof(bytes);
        result = f_read(&file, bytes, size, &received);
        if ((result != FR_OK) || (received == 0u)) {
            break;
        }
        size_t position = 0u;
        for (UINT index = 0u; index < received; ++index) {
            unsigned char character = bytes[index];
            if ((character >= 32u) && (character <= 126u) && (character != '\\')) {
                line[position++] = (char)character;
            } else {
                (void)snprintf(line + position, sizeof(line) - position, "\\x%02x", character);
                position += 4u;
            }
        }
        line[position] = '\0';
        total += received;
        if (!reply(line, context)) {
            interrupted = true;
            break;
        }
    }
    bool truncated = (total == limit) && ((FSIZE_t)total < f_size(&file));
    FRESULT close_result = f_close(&file);
    if (interrupted) (void)reply("WARN Cat output interrupted; FatFs status does not confirm complete delivery.", context);
    if (truncated) {
        (void)reply("WARN Output limit reached; remaining file bytes were not displayed.", context);
    }
    pf_cli_report_result(result != FR_OK ? result : close_result, reply, context);
}

/** @brief Create (or append to) a file with @p text, sync and close it; never overwrites an existing file. */
static void write_command(const char *path, const char *text, bool append,
                          pf_storage_worker_reply_fn_t reply, void *context)
{
    FIL file;
    UINT written = 0u;
    UINT size = (UINT)strlen(text);
    FRESULT result = f_open(&file, path, FA_WRITE | (append ? FA_OPEN_APPEND : FA_CREATE_NEW));

    if (result != FR_OK) {
        pf_cli_report_result(result, reply, context);
        return;
    }
    result = f_write(&file, text, size, &written);
    if ((result == FR_OK) && (written != size)) {
        result = FR_DISK_ERR;
    }
    FRESULT sync_result = f_sync(&file);
    FRESULT close_result = f_close(&file);
    if (result == FR_OK) {
        result = sync_result != FR_OK ? sync_result : close_result;
    }
    if (result != FR_OK) {
        (void)reply("WARN Write failed; a new or partially changed file may remain.", context);
    }
    pf_cli_report_result(result, reply, context);
}

/** One parsed `fs` file command; paths are already volume-prefixed. */
typedef struct {
    const char *second; /**< Second argument (text, limit or confirmation); NULL when absent. */
    char path[PF_CLI_VOLUME_PATH_SIZE]; /**< First path argument. */
    char target[PF_CLI_VOLUME_PATH_SIZE]; /**< Second path argument for commands that take one. */
    unsigned long limit; /**< Byte limit for `cat`. */
    pf_storage_worker_reply_fn_t reply; /**< Borrowed output callback. */
    void *context; /**< Borrowed callback context. */
} fs_call_t;

/** What the second argument of a file command means. */
typedef enum {
    FS_SECOND_NONE, /**< Not used. */
    FS_SECOND_PATH, /**< A second absolute path. */
    FS_SECOND_CONFIRM, /**< The literal --confirm. */
    FS_SECOND_TEXT, /**< Text to write. */
    FS_SECOND_LIMIT, /**< Optional decimal byte limit. */
} second_kind_t;

/** Handler for one validated file command. */
typedef void (*fs_handler_t)(const fs_call_t *call);

/** One `fs` command that needs a mounted volume and a path. */
typedef struct {
    const char *name; /**< Command word. */
    uint8_t min_count; /**< Fewest tokens, including the command word. */
    uint8_t max_count; /**< Most tokens, including the command word. */
    second_kind_t second; /**< Meaning of the second argument. */
    bool default_root; /**< A missing path argument means the volume root. */
    fs_handler_t handler; /**< Executes the command. */
} fs_command_t;

static void do_ls(const fs_call_t *call)
{
    list_command(call->path, call->reply, call->context);
}

static void do_stat(const fs_call_t *call)
{
    stat_command(call->path, call->reply, call->context);
}

static void do_cat(const fs_call_t *call)
{
    cat_command(call->path, call->limit, call->reply, call->context);
}

static void do_write(const fs_call_t *call)
{
    write_command(call->path, call->second, false, call->reply, call->context);
}

static void do_append(const fs_call_t *call)
{
    write_command(call->path, call->second, true, call->reply, call->context);
}

static void do_mkdir(const fs_call_t *call)
{
    pf_cli_report_result(f_mkdir(call->path), call->reply, call->context);
}

static void do_mv(const fs_call_t *call)
{
    pf_cli_report_result(f_rename(call->path, call->target), call->reply, call->context);
}

static void do_rm(const fs_call_t *call)
{
    FILINFO info;
    FRESULT result = f_stat(call->path, &info);

    if ((result == FR_OK) && ((info.fattrib & AM_DIR) != 0u)) {
        (void)call->reply("ERR Refusing directory removal; fs rm only removes files.", call->context);
    } else {
        pf_cli_report_result(result == FR_OK ? f_unlink(call->path) : result, call->reply, call->context);
    }
}

/** Commands that need a mounted volume; order is irrelevant, lookup is by name. */
static const fs_command_t fs_commands[] = {
    { "ls", 1u, 2u, FS_SECOND_NONE, true, do_ls },
    { "stat", 2u, 2u, FS_SECOND_NONE, false, do_stat },
    { "mkdir", 2u, 2u, FS_SECOND_NONE, false, do_mkdir },
    { "cat", 2u, 3u, FS_SECOND_LIMIT, false, do_cat },
    { "write", 3u, 3u, FS_SECOND_TEXT, false, do_write },
    { "append", 3u, 3u, FS_SECOND_TEXT, false, do_append },
    { "mv", 3u, 3u, FS_SECOND_PATH, false, do_mv },
    { "rm", 3u, 3u, FS_SECOND_CONFIRM, false, do_rm },
};

/** @brief Parse `cat` byte limit text into 1..PF_CLI_CAT_MAX. */
static bool parse_cat_limit(const char *text, unsigned long *limit)
{
    char *end;

    if ((text[0] < '0') || (text[0] > '9')) {
        return false;
    }
    *limit = strtoul(text, &end, 10);
    return (*end == '\0') && (*limit >= 1u) && (*limit <= PF_CLI_CAT_MAX);
}

/** @brief Report total and free capacity in bytes. */
static void df_command(pf_storage_worker_reply_fn_t reply, void *context)
{
    DWORD free_clusters;
    FATFS *filesystem;
    FRESULT result = f_getfree(PF_FATFS_VOLUME_PATH, &free_clusters, &filesystem);

    if (result == FR_OK) {
        char line[PF_STORAGE_WORKER_REPLY_SIZE];
        (void)snprintf(line, sizeof(line), "total=%llu bytes free=%llu bytes",
                       (unsigned long long)(filesystem->n_fatent - 2u) * filesystem->csize * FF_MAX_SS,
                       (unsigned long long)free_clusters * filesystem->csize * FF_MAX_SS);
        (void)reply(line, context);
    } else {
        pf_cli_report_result(result, reply, context);
    }
}

/** @brief Validate and run one table-driven file command; the volume is mounted and Pico-owned. */
static void file_command(const char *operation, uint16_t count, const char *first, const char *second,
                         pf_storage_worker_reply_fn_t reply, void *context)
{
    fs_call_t call = { .second = second, .limit = PF_CLI_CAT_DEFAULT, .reply = reply, .context = context };
    const fs_command_t *command = NULL;

    for (size_t index = 0u; index < PF_CLI_ARRAY_SIZE(fs_commands); ++index) {
        if (strcmp(operation, fs_commands[index].name) == 0) {
            command = &fs_commands[index];
            break;
        }
    }
    if ((command == NULL) || (count < command->min_count) || (count > command->max_count) ||
        ((command->second == FS_SECOND_CONFIRM) && (strcmp(second, "--confirm") != 0))) {
        (void)reply("ERR Invalid arguments. Use help fs. Removal requires fs rm <path> --confirm.", context);
        return;
    }
    if ((count == 1u) && command->default_root) {
        first = "/";
    }
    if (!valid_path(first) || ((command->second == FS_SECOND_PATH) && !valid_path(second))) {
        (void)reply("ERR Invalid path: use absolute ASCII paths without drive prefixes, wildcards, . or .. segments "
                    "(max 100 bytes).", context);
        return;
    }
    if ((command->second == FS_SECOND_LIMIT) && (count == 3u) && !parse_cat_limit(second, &call.limit)) {
        (void)reply("ERR Cat limit must be 1..4096 bytes.", context);
        return;
    }
    volume_path(first, call.path);
    if (command->second == FS_SECOND_PATH) {
        volume_path(second, call.target);
    }
    command->handler(&call);
}

void pf_cli_fs_execute(const char *operation, uint16_t count, const char *first, const char *second,
                       pf_storage_worker_reply_fn_t reply, void *context)
{
    if ((operation == NULL) || (reply == NULL)) {
        return;
    }
    if (strcmp(operation, "df") == 0) {
        if (count == 1u) {
            df_command(reply, context);
        } else {
            (void)reply("ERR Invalid arguments. Use help fs.", context);
        }
    } else {
        file_command(operation, count, first, second, reply, context);
    }
}

static void fs_binding_command(EmbeddedCli *instance, char *args, void *context)
{
    (void)instance;
    (void)context;
    if (!fs_dispatch.submit("fs", args != NULL ? args : "", fs_dispatch.context)) {
        pf_cli_print("ERR Storage busy or command too long; wait for completion and retry.");
    } else {
        pf_cli_print("QUEUED Storage command; check the result below.");
    }
}

bool pf_cli_fs_register(pf_cli_worker_submit_fn_t submit, void *context)
{
    if (submit == NULL) return false;
    fs_dispatch.submit = submit;
    fs_dispatch.context = context;
    return pf_cli_add_binding((CliCommandBinding){ "fs", PF_CLI_FS_HELP, false, NULL, fs_binding_command });
}
