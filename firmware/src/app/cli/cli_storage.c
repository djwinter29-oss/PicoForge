/** @file CLI storage-status and filesystem command execution and replies. */
#include "cli_storage.h"
#include "middleware/storage/fatfs_volume.h"
#include "middleware/storage/storage.h"
#include "embedded_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    (void)snprintf(line, sizeof(line), "FatFs: %s (%u)",
                   code < sizeof(names) / sizeof(names[0]) ? names[code] : "UNKNOWN", code);
    (void)reply(line, context);
}

static bool valid_path(const char *path)
{
    const char *segment;

    if ((path == NULL) || (path[0] != '/') || (strlen(path) > 100u)) {
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

static void volume_path(const char *path, char target[104])
{
    (void)snprintf(target, 104u, "%s%s", PF_FATFS_VOLUME_PATH, path);
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
                   states[status.state], host_owned ? "USB" : "Pico",
                   status.medium == PF_STORAGE_MEDIUM_SDCARD ? "sd" : "none",
                   status.bus == PF_STORAGE_BUS_SPI ? "spi" : status.bus == PF_STORAGE_BUS_SDIO ? "sdio" : "unknown",
                   status.state == PF_STORAGE_STATE_ABSENT ? "no" : status.presence_known ? "yes" : "unknown",
                   (unsigned long)status.bus_clock_hz, (unsigned long)status.sector_count,
                   (unsigned long long)status.sector_count * 512u,
                   operations[status.last_failure], (unsigned)status.last_failure_result);
    (void)reply(line, context);
}

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

static void cat_command(const char *path, unsigned long limit,
                        pf_storage_worker_reply_fn_t reply, void *context)
{
    FIL file;
    unsigned char bytes[48];
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

void pf_cli_storage_execute(bool host_owned, const char *family, const char *arguments,
                            pf_storage_worker_reply_fn_t reply, void *context)
{
    char tokens[PF_STORAGE_WORKER_ARGS_SIZE] = { 0 };
    char path[104];
    char target[104];
    const char *operation;
    const char *first;
    const char *second;
    uint16_t count;
    unsigned long limit = 1024u;

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
    first = embeddedCliGetToken(tokens, 2u);
    second = embeddedCliGetToken(tokens, 3u);
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
    bool formatting = strcmp(operation, "format") == 0;
    if (formatting && (count != 2u || strcmp(first, "--confirm") != 0)) {
        (void)reply("Usage: fs format --confirm (destroys all files on selected logical volume)", context);
        return;
    }
    if (formatting || ((count == 1u) && (strcmp(operation, "mount") == 0))) {
        if (pf_fatfs_volume_is_mounted()) {
            (void)reply(formatting ? "Unmount first with fs unmount; format refused." :
                       "Filesystem already mounted.", context);
            return;
        }
        if (pf_storage_initialize() != PF_BLOCK_OK) {
            (void)reply("Mount failed: storage initialization or board configuration unavailable.", context);
            return;
        }
        const pf_block_device_t *device = pf_storage_firmware_device();
        FRESULT result;
        if (formatting) {
            result = pf_fatfs_volume_format(device);
            if (result != FR_OK) {
                (void)reply("Format failed; volume contents may be partially changed.", context);
                report_result(result, reply, context);
                return;
            }
            (void)reply("Filesystem formatted; mounting.", context);
        }
        pf_fatfs_volume_result_t storage_result = pf_fatfs_volume_mount(device, &result);
        if (storage_result == PF_FATFS_VOLUME_OK) {
            (void)reply("Filesystem mounted.", context);
        } else if (storage_result == PF_FATFS_VOLUME_ALREADY_MOUNTED) {
            (void)reply("Filesystem already mounted.", context);
        } else if (storage_result == PF_FATFS_VOLUME_FILESYSTEM_ERROR) {
            report_result(result, reply, context);
        } else {
            (void)reply("Mount failed: invalid block-device descriptor.", context);
        }
        return;
    }
    if (!pf_fatfs_volume_is_mounted()) {
        (void)reply("Filesystem not mounted. Run fs mount first.", context);
        return;
    }
    if ((count == 1u) && (strcmp(operation, "unmount") == 0)) {
        FRESULT result;
        if (pf_fatfs_volume_unmount(&result) == PF_FATFS_VOLUME_OK) {
            (void)reply("Filesystem unmounted; MSC handoff is now allowed.", context);
        } else {
            report_result(result, reply, context);
        }
        return;
    }
    if ((count == 1u) && (strcmp(operation, "df") == 0)) {
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
        return;
    }
    if ((strcmp(operation, "ls") == 0) && (count == 1u)) {
        first = "/";
    }
    bool single = ((strcmp(operation, "ls") == 0) && (count <= 2u)) ||
                  ((strcmp(operation, "stat") == 0) && (count == 2u)) ||
                  ((strcmp(operation, "mkdir") == 0) && (count == 2u));
    bool cat = (strcmp(operation, "cat") == 0) && ((count == 2u) || (count == 3u));
    bool write = ((strcmp(operation, "write") == 0) || (strcmp(operation, "append") == 0)) && (count == 3u);
    bool move = (strcmp(operation, "mv") == 0) && (count == 3u);
    bool remove = (strcmp(operation, "rm") == 0) && (count == 3u) && (strcmp(second, "--confirm") == 0);
    if (!(single || cat || write || move || remove)) {
        (void)reply("Invalid arguments. Use help fs. Removal requires fs rm <path> --confirm.", context);
        return;
    }
    if (!valid_path(first) || (move && !valid_path(second))) {
        (void)reply("Invalid path: use absolute ASCII paths without drive prefixes, wildcards, . or .. segments (max 100 bytes).", context);
        return;
    }
    if (cat && (count == 3u)) {
        if ((second[0] == '\0') || (strspn(second, "0123456789") != strlen(second))) {
            (void)reply("Cat limit must be 1..4096 bytes.", context);
            return;
        }
        limit = strtoul(second, NULL, 10);
        if ((limit == 0u) || (limit > 4096u)) {
            (void)reply("Cat limit must be 1..4096 bytes.", context);
            return;
        }
    }
    volume_path(first, path);
    if (strcmp(operation, "ls") == 0) {
        list_command(path, reply, context);
    } else if (strcmp(operation, "stat") == 0) {
        stat_command(path, reply, context);
    } else if (cat) {
        cat_command(path, limit, reply, context);
    } else if (write) {
        write_command(path, second, strcmp(operation, "append") == 0, reply, context);
    } else if (move) {
        volume_path(second, target);
        report_result(f_rename(path, target), reply, context);
    } else if (remove) {
        FILINFO info;
        FRESULT result = f_stat(path, &info);
        if ((result == FR_OK) && ((info.fattrib & AM_DIR) != 0u)) {
            (void)reply("Refusing directory removal; fs rm only removes files.", context);
        } else {
            report_result(result == FR_OK ? f_unlink(path) : result, reply, context);
        }
    } else {
        report_result(f_mkdir(path), reply, context);
    }
}