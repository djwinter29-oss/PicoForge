#ifndef STORAGE_SERIALIZATION_TASK_H
#define STORAGE_SERIALIZATION_TASK_H
#include <pthread.h>
extern pthread_mutex_t creation_lock;
#define taskENTER_CRITICAL() pthread_mutex_lock(&creation_lock)
#define taskEXIT_CRITICAL() pthread_mutex_unlock(&creation_lock)
#endif
