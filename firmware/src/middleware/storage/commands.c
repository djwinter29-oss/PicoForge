#include "commands.h"
#include "sd_card_block.h"
#include "embedded_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void report_result(FRESULT result, pf_storage_reply_t reply, void *context)
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
    (void)snprintf(target, 104u, "%s%s", PF_STORAGE_VOLUME, path);
}

static void status_command(bool host_owned, pf_storage_reply_t reply, void *context)
{
    pf_sd_card_info_t info;
    char line[PF_STORAGE_REPLY_SIZE];

    pf_sd_card_get_info(&info);
    (void)snprintf(line, sizeof(line),
                   "owner=%s transport=%s present=%s initialized=%u mounted=%u clock=%lu Hz sectors=%lu bytes=%llu",
                   host_owned ? "USB" : "Pico",
                   pf_sd_card_transport_name(),
                   info.presence_known ? (info.present ? "yes" : "no") : "unknown",
                   (unsigned)info.initialized, (unsigned)pf_storage_is_mounted(),
                   (unsigned long)info.bus_clock_hz, (unsigned long)info.block_count,
                   (unsigned long long)info.block_count * PF_SD_CARD_BLOCK_SIZE);
    (void)reply(line, context);
}

static void list_command(const char *path, pf_storage_reply_t reply, void *context)
{
    DIR directory;
    FILINFO info;
    char line[PF_STORAGE_REPLY_SIZE];
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

static void stat_command(const char *path, pf_storage_reply_t reply, void *context)
{
    FILINFO info;
    char line[PF_STORAGE_REPLY_SIZE];
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
                        pf_storage_reply_t reply, void *context)
{
    FIL file;
    unsigned char bytes[48];
    char line[PF_STORAGE_REPLY_SIZE];
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
                          pf_storage_reply_t reply, void *context)
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

void pf_storage_command_execute(const pf_sd_card_config_t *config, bool host_owned,
                                const char *family, const char *arguments,
                                pf_storage_reply_t reply, void *context)
{
    char tokens[PF_STORAGE_COMMAND_ARGS_SIZE] = { 0 };
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
    if ((strcmp(family, "sd") == 0) && (count == 1u) && (strcmp(operation, "status") == 0)) {
        status_command(host_owned, reply, context);
        return;
    }
    if ((strcmp(family, "fs") != 0) || (count == 0u)) {
        (void)reply("Usage: sd status | fs <mount|unmount|ls|stat|cat|df|mkdir|write|append|mv|rm>", context);
        return;
    }
    if (host_owned) {
        (void)reply("Card owned by USB. Safely eject and disable MSC first.", context);
        return;
    }
    if ((count == 1u) && (strcmp(operation, "mount") == 0)) {
        FRESULT result;
        pf_storage_result_t storage_result = pf_storage_mount(config, &result);
        if (storage_result == PF_STORAGE_OK) {
            (void)reply("Filesystem mounted.", context);
        } else if (storage_result == PF_STORAGE_ALREADY_MOUNTED) {
            (void)reply("Filesystem already mounted.", context);
        } else if (storage_result == PF_STORAGE_FILESYSTEM_ERROR) {
            report_result(result, reply, context);
        } else {
            (void)reply("Mount failed: SD initialization or board configuration unavailable.", context);
        }
        return;
    }
    if (!pf_storage_is_mounted()) {
        (void)reply("Filesystem not mounted. Run fs mount first.", context);
        return;
    }
    if ((count == 1u) && (strcmp(operation, "unmount") == 0)) {
        FRESULT result;
        if (pf_storage_unmount(&result) == PF_STORAGE_OK) {
            (void)reply("Filesystem unmounted; MSC handoff is now allowed.", context);
        } else {
            report_result(result, reply, context);
        }
        return;
    }
    if ((count == 1u) && (strcmp(operation, "df") == 0)) {
        DWORD free_clusters;
        FATFS *filesystem;
        FRESULT result = f_getfree(PF_STORAGE_VOLUME, &free_clusters, &filesystem);
        if (result == FR_OK) {
            char line[PF_STORAGE_REPLY_SIZE];
            (void)snprintf(line, sizeof(line), "total=%llu bytes free=%llu bytes",
                           (unsigned long long)(filesystem->n_fatent - 2u) * filesystem->csize * PF_SD_CARD_BLOCK_SIZE,
                           (unsigned long long)free_clusters * filesystem->csize * PF_SD_CARD_BLOCK_SIZE);
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