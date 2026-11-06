// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

#pragma once
#include <pthread.h>
extern pthread_mutex_t creation_lock;
#define taskENTER_CRITICAL() pthread_mutex_lock(&creation_lock)
#define taskEXIT_CRITICAL() pthread_mutex_unlock(&creation_lock)
