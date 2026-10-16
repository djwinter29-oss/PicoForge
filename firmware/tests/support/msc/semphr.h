#pragma once
#include <pthread.h>
#include "unity.h"
typedef pthread_mutex_t StaticSemaphore_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *m)
{
    pthread_mutexattr_t a;
    TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_init(&a));
    TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE));
    TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(m, &a));
    TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_destroy(&a));
    return m;
}
#define xSemaphoreTakeRecursive(m, timeout) (((timeout) == 0 ? pthread_mutex_trylock(m) : pthread_mutex_lock(m)) == 0)
#define xSemaphoreGiveRecursive(m) (pthread_mutex_unlock(m) == 0)
