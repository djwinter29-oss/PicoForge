#ifndef PICO_MUTEX_H
#define PICO_MUTEX_H

#include <stdbool.h>
#include <stdint.h>

typedef struct { bool locked; } mutex_t;

#define auto_init_mutex(name) static mutex_t name

/* Host model of a non-recursive mutex: a contended wait times out immediately. */
static inline bool mutex_enter_timeout_ms(mutex_t *mutex, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (mutex->locked) {
        return false;
    }
    mutex->locked = true;
    return true;
}

static inline void mutex_exit(mutex_t *mutex)
{
    mutex->locked = false;
}

#endif
