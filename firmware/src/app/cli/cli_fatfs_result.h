// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

/**
 * @file
 * @brief Private FatFs result reporting used by storage-backed CLI commands.
 */
#ifndef PF_CLI_FATFS_RESULT_H
#define PF_CLI_FATFS_RESULT_H

#include "app/storage/storage_worker.h"
#include "ff.h"

/**
 * @brief Reply with a named FatFs result and numeric code.
 * @param result FatFs result.
 * @param reply Output callback; must not be NULL.
 * @param context Borrowed callback context.
 */
void pf_cli_report_result(FRESULT result, pf_storage_worker_reply_fn_t reply, void *context);

#endif
