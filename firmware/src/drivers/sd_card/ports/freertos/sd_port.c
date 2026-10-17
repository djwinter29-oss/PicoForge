#include "sd_port.h"
#include "config/config.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

static StaticSemaphore_t card_mutex_storage;
static SemaphoreHandle_t card_mutex;

bool pf_sd_port_init(const pf_sd_config_t *config)
{
#if PF_CONFIG_SD_CARD_INTERRUPT_WAIT
    return config != NULL && config->transport.mode == PF_STORAGE_TRANSPORT_NATIVE_SD &&
           pf_storage_transport_enable_wait(&config->transport);
#else
    (void)config;
    return true;
#endif
}

static void ensure_card_mutex(void)
{
    /* Telemetry may reserve the facade before card initialization. */
    taskENTER_CRITICAL();
    if (card_mutex == NULL) {
        card_mutex = xSemaphoreCreateRecursiveMutexStatic(&card_mutex_storage);
    }
    taskEXIT_CRITICAL();
}

bool pf_sd_port_try_lock(void)
{
    ensure_card_mutex();
    return xSemaphoreTakeRecursive(card_mutex, 0u) == pdTRUE;
}

void pf_sd_port_lock(void)
{
    ensure_card_mutex();
    BaseType_t taken = xSemaphoreTakeRecursive(card_mutex, portMAX_DELAY);
    configASSERT(taken == pdTRUE);
    (void)taken;
}

void pf_sd_port_unlock(void)
{
    BaseType_t given = xSemaphoreGiveRecursive(card_mutex);
    configASSERT(given == pdTRUE);
    (void)given;
}
