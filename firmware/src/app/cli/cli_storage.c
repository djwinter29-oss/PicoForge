/**
 * @file
 * @brief CLI storage-status and filesystem command execution and replies.
 */
#include "cli_storage.h"
#include "platform/storage/fatfs_volume.h"
#include "platform/storage/storage.h"
#include "embedded_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/** Element count of a true array. No shared helper exists in the tree. */
#define PF_CLI_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

_Static_assert(PF_CLI_CAT_CHUNK * 4u < PF_STORAGE_WORKER_REPLY_SIZE, "cat escapes must fit one reply line");
_Static_assert(PF_CLI_VOLUME_PATH_SIZE >= sizeof(PF_FATFS_VOLUME_PATH) + PF_CLI_PATH_MAX, "volume path buffer");

/** @brief Name for an enum value, or "unknown" when it is outside the table. */
static const char *name_or_unknown(const char *const *names, size_t count, unsigned index)
{
    return index < count ? names[index] : "unknown";
}

/** @brief Reply with the FatFs result name and number; always replies once, even after a cancelled listing. */
static void report_result(FRESULT result, pf_storage_worker_reply_fn_t reply, void *context)
{
    static const char *const names[] = {
        "OK", "DISK_ERR", "INT_ERR", "NOT_READY", "NO_FILE", "NO_PATH",
        "INVALID_NAME", "DENIED", "EXIST", "INVALID_OBJECT", "WRITE_PROTECTED",
        "INVALID_DRIVE", "NOT_ENABLED", "NO_FILESYSTEM", "MKFS_ABORTED",
        "TIMEOUT", "LOCKED", "NOT_ENOUGH_CORE", "TOO_MANY_OPEN_FILES",
        "INVALID_PARAMETER"
    };
    char line[80];
    unsigned code = (unsigned)result;

    /* ponytail: names track FatFs 0.15 FRESULT; a newer code prints UNKNOWN with its number until this list grows. */
    (void)snprintf(line, sizeof(line), "FatFs: %s (%u)",
                   code < PF_CLI_ARRAY_SIZE(names) ? names[code] : "UNKNOWN", code);
    (void)reply(line, context);
}

/** @brief True for an absolute printable-ASCII path without FAT-reserved characters or . and .. segments. */
static bool valid_path(const char *path)
{
    const char *segment;

    if ((path == NULL) || (path[0] != '/') || (strlen(path) > PF_CLI_PATH_MAX)) {
        return false;
    }
    segment = path + 1u;
    for (const char *position = segment; ; ++position) {
        unsigned char character = (unsigned char)*position;
        if ((character == '\0') || (character == '/')) {
            size_t size = (size_t)(position - segment);
            if (((size == 1u) && (segment[0] == '.')) ||
                ((size == 2u) && (segment[0] == '.') && (segment[1] == '.'))) {
                return false;
            }
            if (character == '\0') {
                return true;
            }
            segment = position + 1u;
        } else if ((character < 32u) || (character > 126u) ||
                   (strchr(":\\*?\"<>|", character) != NULL)) {
            return false;
        }
    }
}

/** @brief Prefix @p path with the FatFs volume; @p path was validated to fit. */
static void volume_path(const char *path, char target[PF_CLI_VOLUME_PATH_SIZE])
{
    (void)snprintf(target, PF_CLI_VOLUME_PATH_SIZE, "%s%s", PF_FATFS_VOLUME_PATH, path);
}

/** @brief Report the backend-neutral storage status on one line. */
static void status_command(bool host_owned, pf_storage_worker_reply_fn_t reply, void *context)
{
    static const char *const states[] = {
        "not_configured", "busy", "absent", "uninitialized", "ready", "mounted", "usb",
    };
    static const char *const operations[] = {"none", "init", "read", "write", "sync", "msc_acquire"};
    pf_storage_status_t status;
    char line[PF_STORAGE_WORKER_REPLY_SIZE];

    pf_storage_get_status(&status);
    (void)snprintf(line, sizeof(line),
                   "state=%s owner=%s medium=%s bus=%s present=%s clock=%lu Hz sectors=%lu bytes=%llu "
                   "last_error=%s:%u",
                   name_or_unknown(states, PF_CLI_ARRAY_SIZE(states), (unsigned)status.state),
                   host_owned ? "USB" : "Pico",
                   status.medium == PF_STORAGE_MEDIUM_SDCARD ? "sd" : "none",
                   status.bus == PF_STORAGE_BUS_SPI ? "spi" :
                   status.bus == PF_STORAGE_BUS_SDIO ? "sdio" : "unknown",
                   status.state == PF_STORAGE_STATE_ABSENT ? "no" : status.presence_known ? "yes" : "unknown",
                   (unsigned long)status.bus_clock_hz, (unsigned long)status.sector_count,
                   (unsigned long long)status.sector_count * FF_MAX_SS,
                   name_or_unknown(operations, PF_CLI_ARRAY_SIZE(operations), (unsigned)status.last_failure),
                   (unsigned)status.last_failure_result);
    (void)reply(line, context);
}

/**
 * @brief List one directory, one entry per reply line, then the final FatFs result.
 * @note Names are UTF-8 (FF_LFN_UNICODE 2), so bytes of 128 and above pass through; only control characters
 *       are replaced.
 */
static void list_command(const char *path, pf_storage_worker_reply_fn_t reply, void *context)
{
    DIR directory;
    FILINFO info;
    char line[PF_STORAGE_WORKER_REPLY_SIZE];
    FRESULT result = f_opendir(&directory, path);

    if (result != FR_OK) {
        report_result(result, reply, context);
        return;
    }
    for (;;) {
        result = f_readdir(&directory, &info);
        if ((result != FR_OK) || (info.fname[0] == '\0')) {
            break;
        }
        (void)snprintf(line, sizeof(line), "%c %llu %.200s",
                       (info.fattrib & AM_DIR) != 0u ? 'd' : '-',
                       (unsigned long long)info.fsize, info.fname);
        for (char *position = line; *position != '\0'; ++position) {
            if (((unsigned char)*position < 32u) || ((unsigned char)*position == 127u)) {
                *position = '?';
            }
        }
        if (!reply(line, context)) {
            break;
        }
    }
    FRESULT close_result = f_closedir(&directory);
    report_result(result != FR_OK ? result : close_result, reply, context);
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
        report_result(result, reply, context);
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

    if (result != FR_OK) {
        report_result(result, reply, context);
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
            break;
        }
    }
    bool truncated = (total == limit) && ((FSIZE_t)total < f_size(&file));
    FRESULT close_result = f_close(&file);
    if (truncated) {
        (void)reply("Output limit reached; remaining file bytes were not displayed.", context);
    }
    report_result(result != FR_OK ? result : close_result, reply, context);
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
        report_result(result, reply, context);
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
        (void)reply("Write failed; a new or partially changed file may remain.", context);
    }
    report_result(result, reply, context);
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
    report_result(f_mkdir(call->path), call->reply, call->context);
}

static void do_mv(const fs_call_t *call)
{
    report_result(f_rename(call->path, call->target), call->reply, call->context);
}

static void do_rm(const fs_call_t *call)
{
    FILINFO info;
    FRESULT result = f_stat(call->path, &info);

    if ((result == FR_OK) && ((info.fattrib & AM_DIR) != 0u)) {
        (void)call->reply("Refusing directory removal; fs rm only removes files.", call->context);
    } else {
        report_result(result == FR_OK ? f_unlink(call->path) : result, call->reply, call->context);
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

/** @brief Mount or format; the caller verified Pico ownership. */
static void mount_or_format(bool formatting, pf_storage_worker_reply_fn_t reply, void *context)
{
    const pf_block_device_t *device;
    FRESULT result;
    pf_fatfs_volume_result_t storage_result;

    if (pf_fatfs_volume_is_mounted()) {
        (void)reply(formatting ? "Unmount first with fs unmount; format refused." : "Filesystem already mounted.",
                    context);
        return;
    }
    if (pf_storage_initialize() != PF_BLOCK_OK) {
        (void)reply("Mount failed: storage initialization or board configuration unavailable.", context);
        return;
    }
    device = pf_storage_firmware_device();
    if (formatting) {
        result = pf_fatfs_volume_format(device);
        if (result != FR_OK) {
            (void)reply("Format failed; volume contents may be partially changed.", context);
            report_result(result, reply, context);
            return;
        }
        (void)reply("Filesystem formatted; mounting.", context);
    }
    storage_result = pf_fatfs_volume_mount(device, &result);
    if (storage_result == PF_FATFS_VOLUME_OK) {
        (void)reply("Filesystem mounted.", context);
    } else if (storage_result == PF_FATFS_VOLUME_ALREADY_MOUNTED) {
        (void)reply("Filesystem already mounted.", context);
    } else if (storage_result == PF_FATFS_VOLUME_FILESYSTEM_ERROR) {
        report_result(result, reply, context);
    } else {
        (void)reply("Mount failed: invalid block-device descriptor.", context);
    }
}

/** @brief Unmount the volume, allowing MSC handoff. */
static void unmount_command(pf_storage_worker_reply_fn_t reply, void *context)
{
    FRESULT result;

    if (pf_fatfs_volume_unmount(&result) == PF_FATFS_VOLUME_OK) {
        (void)reply("Filesystem unmounted; MSC handoff is now allowed.", context);
    } else {
        report_result(result, reply, context);
    }
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
        report_result(result, reply, context);
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
        (void)reply("Invalid arguments. Use help fs. Removal requires fs rm <path> --confirm.", context);
        return;
    }
    if ((count == 1u) && command->default_root) {
        first = "/";
    }
    if (!valid_path(first) || ((command->second == FS_SECOND_PATH) && !valid_path(second))) {
        (void)reply("Invalid path: use absolute ASCII paths without drive prefixes, wildcards, . or .. segments "
                    "(max 100 bytes).", context);
        return;
    }
    if ((command->second == FS_SECOND_LIMIT) && (count == 3u) && !parse_cat_limit(second, &call.limit)) {
        (void)reply("Cat limit must be 1..4096 bytes.", context);
        return;
    }
    volume_path(first, call.path);
    if (command->second == FS_SECOND_PATH) {
        volume_path(second, call.target);
    }
    command->handler(&call);
}

void pf_cli_storage_execute(bool host_owned, const char *family, const char *arguments,
                            pf_storage_worker_reply_fn_t reply, void *context)
{
    char tokens[PF_STORAGE_WORKER_ARGS_SIZE] = { 0 };
    const char *operation; /* host_owned is the caller's snapshot; see the header for the no-transition rule. */
    uint16_t count;
    bool formatting;

    if ((reply == NULL) || (family == NULL) || (arguments == NULL)) {
        return;
    }
    if (strlen(arguments) > sizeof(tokens) - 2u) {
        (void)reply("Arguments too long.", context);
        return;
    }
    memcpy(tokens, arguments, strlen(arguments));
    embeddedCliTokenizeArgs(tokens);
    count = embeddedCliGetTokenCount(tokens);
    operation = embeddedCliGetToken(tokens, 1u);
    if ((strcmp(family, "storage") == 0) && (count == 1u) && (strcmp(operation, "status") == 0)) {
        status_command(host_owned, reply, context);
        return;
    }
    if ((strcmp(family, "fs") != 0) || (count == 0u)) {
        (void)reply("Usage: storage status | fs <mount|unmount|format|ls|stat|cat|df|mkdir|write|append|mv|rm>",
                    context);
        return;
    }
    if (host_owned) {
        (void)reply("Media owned by USB. Safely eject and disable MSC first.", context);
        return;
    }
    formatting = strcmp(operation, "format") == 0;
    if (formatting && ((count != 2u) || (strcmp(embeddedCliGetToken(tokens, 2u), "--confirm") != 0))) {
        (void)reply("Usage: fs format --confirm (destroys all files on selected logical volume)", context);
        return;
    }
    if (formatting || ((count == 1u) && (strcmp(operation, "mount") == 0))) {
        mount_or_format(formatting, reply, context);
        return;
    }
    if (!pf_fatfs_volume_is_mounted()) {
        (void)reply("Filesystem not mounted. Run fs mount first.", context);
    } else if ((count == 1u) && (strcmp(operation, "unmount") == 0)) {
        unmount_command(reply, context);
    } else if ((count == 1u) && (strcmp(operation, "df") == 0)) {
        df_command(reply, context);
    } else {
        file_command(operation, count, embeddedCliGetToken(tokens, 2u), embeddedCliGetToken(tokens, 3u),
                     reply, context);
    }
}
