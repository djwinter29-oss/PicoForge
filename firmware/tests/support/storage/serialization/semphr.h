#ifndef STORAGE_SERIALIZATION_SEMPHR_H
#define STORAGE_SERIALIZATION_SEMPHR_H
#include <assert.h>
#include <pthread.h>
typedef pthread_mutex_t StaticSemaphore_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *mutex)
{
    pthread_mutexattr_t attributes;
    assert(!pthread_mutexattr_init(&attributes));
    assert(!pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE));
    assert(!pthread_mutex_init(mutex, &attributes));
    assert(!pthread_mutexattr_destroy(&attributes));
    return mutex;
}
#define xSemaphoreTakeRecursive(mutex, timeout) \
    (((timeout) == 0 ? pthread_mutex_trylock(mutex) : pthread_mutex_lock(mutex)) == 0)
#define xSemaphoreGiveRecursive(mutex) (pthread_mutex_unlock(mutex) == 0)
#endif
