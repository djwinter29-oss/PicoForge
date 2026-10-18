/** @file Application SD/filesystem command execution and replies. */
#include "storage_commands.h"
#include "app/storage/fatfs_volume.h"
#include "sd_media.h"
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
    (void)snprintf(target, 104u, "%s%s", PF_FATFS_VOLUME_PATH, path);
}

static void status_command(bool host_owned, pf_storage_reply_t reply, void *context)
{
    pf_sd_info_t info;
    char line[PF_STORAGE_REPLY_SIZE];

    pf_sd_get_info(&info);
    (void)snprintf(line, sizeof(line),
                   "owner=%s transport=%s present=%s initialized=%u mounted=%u clock=%lu Hz sectors=%lu bytes=%llu",
                   host_owned ? "USB" : "Pico",
                   pf_sd_transport_name(),
                   info.presence_known ? (info.present ? "yes" : "no") : "unknown",
                   (unsigned)info.initialized, (unsigned)pf_fatfs_volume_is_mounted(),
                   (unsigned long)info.bus_clock_hz, (unsigned long)info.block_count,
                   (unsigned long long)info.block_count * PF_SD_BLOCK_SIZE);
    (void)reply(line, context);
}

static const char *detail_error_name(pf_sd_detail_error_t error)
{
    static const char *const names[] = {
        "none", "unsupported_legacy_card", "unsupported_voltage", "initialization_timeout",
        "invalid_r7", "ocr_ccs_failure", "response_crc", "response_index", "card_status",
        "unsupported_four_bit", "high_speed_unsupported", "high_speed_rejected",
        "data_crc", "dma_timeout", "pio_timeout", "card_busy_timeout", "card_removed",
        "host_state", "tx_stall", "rx_stall"
    };
    unsigned index = (unsigned)error;
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : "unknown";
}

static bool register_line(const char *name, const uint8_t *bytes, size_t size,
                          pf_storage_reply_t reply, void *context)
{
    char line[80];
    size_t used = (size_t)snprintf(line, sizeof(line), "%s=", name);
    for (size_t i = 0u; i < size && used + 2u < sizeof(line); ++i) {
        (void)snprintf(line + used, sizeof(line) - used, "%02x", bytes[i]);
        used += 2u;
    }
    return reply(line, context);
}

static void info_command(pf_storage_reply_t reply, void *context)
{
    pf_sd_info_t info;
    char line[PF_STORAGE_REPLY_SIZE];
    pf_sd_get_info(&info);
    if (info.cid_valid) {
        if (!register_line("cid", info.cid, sizeof(info.cid), reply, context)) return;
        uint32_t serial = ((uint32_t)info.cid[9] << 24u) |
                          ((uint32_t)info.cid[10] << 16u) |
                          ((uint32_t)info.cid[11] << 8u) | info.cid[12];
        unsigned date = ((info.cid[13] & 15u) << 8u) | info.cid[14];
        char product[6] = {0};
        for (unsigned i = 0u; i < 5u; ++i) {
            uint8_t c = info.cid[i + 3u];
            product[i] = c >= 32u && c <= 126u ? (char)c : '?';
        }
        (void)snprintf(line, sizeof(line),
            "manufacturer=%u product=%s revision=%u.%u serial=%lu date=%u-%02u",
            info.cid[0], product, info.cid[8] >> 4u, info.cid[8] & 15u,
            (unsigned long)serial, 2000u + (date >> 4u), date & 15u);
        if (!reply(line, context)) return;
    }
    (void)snprintf(line, sizeof(line), "initialized=%u block_size=%u blocks=%lu capacity=%llu",
        (unsigned)info.initialized, PF_SD_BLOCK_SIZE, (unsigned long)info.block_count,
        (unsigned long long)info.block_count * PF_SD_BLOCK_SIZE);
    if (!reply(line, context)) return;
    if (!info.native_details_valid) {
        (void)reply("Native startup details unavailable for this transport or before initialization.", context);
        return;
    }
    if (!register_line("csd", info.csd, sizeof(info.csd), reply, context) ||
        !register_line("scr", info.scr, sizeof(info.scr), reply, context)) return;
    (void)snprintf(line, sizeof(line),
        "ocr=%08lx ccs=%u rca=%04x sd_spec=%u widths=%02x bus_width=%u timing=%s",
        (unsigned long)info.ocr, (unsigned)((info.ocr >> 30u) & 1u), info.rca,
        info.sd_spec, info.supported_bus_widths, info.bus_width,
        info.high_speed ? "high-speed" : "default-speed");
    if (!reply(line, context)) return;
    (void)snprintf(line, sizeof(line),
        "init_requested_hz=%lu init_actual_hz=%lu requested_hz=%lu actual_hz=%lu",
        (unsigned long)info.requested_init_clock_hz, (unsigned long)info.actual_init_clock_hz,
        (unsigned long)info.requested_clock_hz, (unsigned long)info.actual_clock_hz);
    if (!reply(line, context)) return;
    (void)snprintf(line, sizeof(line),
        "error=%s phase=%s high_speed_error=%s init_attempts=%lu retries=%lu fallbacks_50_to_25=%lu",
        detail_error_name(info.detail_error), info.initialization_error ? "init" : "runtime",
        detail_error_name(info.high_speed_error), (unsigned long)info.init_attempts,
        (unsigned long)(info.init_attempts > 0u ? info.init_attempts - 1u : 0u),
        (unsigned long)info.high_speed_fallbacks);
    if (!reply(line, context)) return;
    (void)snprintf(line, sizeof(line),
        "command_crc=%lu data_crc=%lu command_timeouts=%lu data_timeouts=%lu dma_timeouts=%lu",
        (unsigned long)info.errors.command_crc_errors, (unsigned long)info.errors.data_crc_errors,
        (unsigned long)info.errors.command_timeouts, (unsigned long)info.errors.data_timeouts,
        (unsigned long)info.errors.dma_timeouts);
    if (!reply(line, context)) return;
    (void)snprintf(line, sizeof(line), "busy_timeouts=%lu pio_timeouts=%lu tx_stalls=%lu rx_stalls=%lu",
        (unsigned long)info.errors.busy_timeouts, (unsigned long)info.errors.pio_timeouts,
        (unsigned long)info.errors.tx_stalls, (unsigned long)info.errors.rx_stalls);
    (void)reply(line, context);
}

static void profile_command(const char *action, bool host_owned,
                            pf_storage_reply_t reply, void *context)
{
    if (action != NULL && strcmp(action, "status") != 0) {
        if (host_owned) {
            (void)reply("Profiling changes require firmware card ownership.", context);
            return;
        }
        if (strcmp(action, "on") == 0 || strcmp(action, "off") == 0) {
            pf_sd_set_profiling_enabled(strcmp(action, "on") == 0);
        } else if (strcmp(action, "reset") == 0) {
            pf_sd_reset_profile();
        } else {
            (void)reply("Usage: sd profile [status|on|off|reset]", context);
            return;
        }
    }
    static const char *const phase_names[] = { "command", "data", "crc_copy", "busy", "stop", "recovery" };
    pf_sd_profile_t metrics;
    pf_sd_get_profile(&metrics);
    char line[PF_STORAGE_REPLY_SIZE];
    for (unsigned phase = 0u; phase < PF_SD_PROFILE_PHASE_COUNT; ++phase) {
        if (phase == PF_SD_PROFILE_RECOVERY) {
            (void)snprintf(line, sizeof(line), "recovery_us=reserved calls=reserved");
        } else {
            (void)snprintf(line, sizeof(line), "%s_us=%llu calls=%lu",
                           phase_names[phase], (unsigned long long)metrics.elapsed_us[phase],
                           (unsigned long)metrics.calls[phase]);
        }
        if (!reply(line, context)) {
            return;
        }
    }
    (void)snprintf(line, sizeof(line), "crc_failures=%lu batch_fallbacks=%lu timeouts=%lu recoveries=reserved",
                   (unsigned long)metrics.events[PF_SD_PROFILE_CRC_FAILURE],
                   (unsigned long)metrics.events[PF_SD_PROFILE_BATCH_FALLBACK],
                   (unsigned long)metrics.events[PF_SD_PROFILE_TIMEOUT]);
    (void)reply(line, context);
}

static void diagnostics_command(const char *action, bool host_owned,
                                pf_storage_reply_t reply, void *context)
{
    if ((action != NULL) && (strcmp(action, "status") != 0)) {
        if (strcmp(action, "clear") != 0) {
            (void)reply("Usage: sd diagnostics [status|clear]", context);
            return;
        }
        if (host_owned) {
            (void)reply("Clearing diagnostics requires firmware card ownership.", context);
            return;
        }
        pf_sd_media_clear_diagnostics();
    }

    static const char *const operation_names[] = {
        "none", "init", "read", "write", "sync", "msc_acquire", "msc_release"
    };
    static const char *const result_names[] = {
        "ok", "invalid_argument", "config_locked", "not_configured",
        "init_failed", "not_ready", "io_error"
    };
    pf_sd_media_diagnostics_t diagnostics;
    pf_sd_media_get_diagnostics(&diagnostics);
    unsigned operation = (unsigned)diagnostics.operation;
    unsigned result = (unsigned)diagnostics.result;
    char line[PF_STORAGE_REPLY_SIZE];
    (void)snprintf(
        line, sizeof(line),
        "sequence=%lu operation=%s result=%s owner=%s transport=%s init_stage=%lu",
        (unsigned long)diagnostics.sequence,
        operation < sizeof(operation_names) / sizeof(operation_names[0])
            ? operation_names[operation] : "unknown",
        result < sizeof(result_names) / sizeof(result_names[0])
            ? result_names[result] : "unknown",
        diagnostics.owner == PF_SD_MEDIA_OWNER_USB_MSC ? "USB" : "Pico",
        diagnostics.transport == PF_SD_TRANSPORT_SPI ? "spi" : "sdio",
        (unsigned long)diagnostics.transport_init_stage);
    if (!reply(line, context)) {
        return;
    }
    (void)snprintf(
        line, sizeof(line),
        "crc_failures=%lu batch_fallbacks=%lu timeouts=%lu recoveries=reserved",
        (unsigned long)diagnostics.profile.events[PF_SD_PROFILE_CRC_FAILURE],
        (unsigned long)diagnostics.profile.events[PF_SD_PROFILE_BATCH_FALLBACK],
        (unsigned long)diagnostics.profile.events[PF_SD_PROFILE_TIMEOUT]);
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

void pf_storage_command_execute(bool host_owned, const char *family, const char *arguments,
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
    if ((strcmp(family, "sd") == 0) && (count == 1u) && (strcmp(operation, "info") == 0)) {
        info_command(reply, context);
        return;
    }
    if (strcmp(family, "sd") == 0 && count >= 1u && count <= 2u &&
        strcmp(operation, "profile") == 0) {
        profile_command(count == 2u ? first : NULL, host_owned, reply, context);
        return;
    }
    if (strcmp(family, "sd") == 0 && count >= 1u && count <= 2u &&
        strcmp(operation, "diagnostics") == 0) {
        diagnostics_command(count == 2u ? first : NULL, host_owned, reply, context);
        return;
    }
    if ((strcmp(family, "fs") != 0) || (count == 0u)) {
        (void)reply("Usage: sd <status|info|profile|diagnostics> | fs <mount|unmount|format|ls|stat|cat|df|mkdir|write|append|mv|rm>", context);
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
        if (pf_sd_media_initialize() != PF_SD_OK) {
            (void)reply("Mount failed: SD initialization or board configuration unavailable.", context);
            return;
        }
        const pf_block_device_t *device = pf_sd_media_firmware_device();
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
            char line[PF_STORAGE_REPLY_SIZE];
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