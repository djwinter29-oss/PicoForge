/** @file
 * @brief Interval CPU accounting and static SRAM reservation from Pico SDK linker symbols.
 */
#include "runtime_stats.h"
#include <stdbool.h>

#include "FreeRTOS.h"
#include "task.h"
#include "pico/time.h"
#include "hardware/regs/addressmap.h"

extern char __end__[], __scratch_x_start__[], __scratch_x_end__[];
extern char __scratch_y_start__[], __scratch_y_end__[];
extern char __StackTop[], __StackBottom[], __StackOneTop[], __StackOneBottom[];

/* Idle run-time and the wall clock must share units: FreeRTOSConfig.h sets portGET_RUN_TIME_COUNTER_VALUE() to
 * time_us_64(). Change both together. */
_Static_assert(configNUMBER_OF_CORES == PF_RUNTIME_CORE_COUNT, "runtime stats report every FreeRTOS core");

#define RUNTIME_STATS_MIN_INTERVAL_US 250000u /**< Shorter reads return the cached result of the last full interval. */

static uint64_t previous_time; /**< Last interval boundary in microseconds. */
static uint64_t previous_idle[PF_RUNTIME_CORE_COUNT]; /**< Per-core idle time at the last boundary. */
static bool sampled; /**< Whether an interval baseline exists. */
/** Cached per-core interval results. */
static uint16_t cpu_usage[PF_RUNTIME_CORE_COUNT] = {PF_RUNTIME_CPU_UNAVAILABLE, PF_RUNTIME_CPU_UNAVAILABLE};

/** @brief Idle-task run time of one core in microseconds. */
static uint64_t idle_time_us(unsigned core)
{
    return ulTaskGetRunTimeCounter(xTaskGetIdleTaskHandleForCore((BaseType_t)core));
}

void pf_runtime_stats_read(pf_runtime_stats_t *stats)
{
    configASSERT(stats != NULL);
    taskENTER_CRITICAL();
    uint64_t now = time_us_64();
    if (!sampled || now - previous_time >= RUNTIME_STATS_MIN_INTERVAL_US) {
        uint64_t elapsed = now - previous_time;
        for (unsigned core = 0u; core < PF_RUNTIME_CORE_COUNT; ++core) {
            uint64_t idle = idle_time_us(core);
            if (sampled) {
                uint64_t idle_elapsed = idle - previous_idle[core];
                /* Adjacent wall/idle samples can skew slightly; saturate at idle instead of underflowing. */
                cpu_usage[core] = idle_elapsed >= elapsed ? 0u :
                    (uint16_t)(((elapsed - idle_elapsed) * 10000u) / elapsed);
            }
            previous_idle[core] = idle;
        }
        previous_time = now;
        sampled = true;
    }
    for (unsigned core = 0u; core < PF_RUNTIME_CORE_COUNT; ++core) {
        stats->cpu_usage_basis_points[core] = cpu_usage[core];
    }
    taskEXIT_CRITICAL();
    stats->ram_total_bytes = SRAM_END - SRAM_BASE;
    stats->ram_used_bytes = (uintptr_t)__end__ - SRAM_BASE +
        ((uintptr_t)__scratch_x_end__ - (uintptr_t)__scratch_x_start__) +
        ((uintptr_t)__scratch_y_end__ - (uintptr_t)__scratch_y_start__) +
        ((uintptr_t)__StackTop - (uintptr_t)__StackBottom) +
        ((uintptr_t)__StackOneTop - (uintptr_t)__StackOneBottom);
    configASSERT(stats->ram_used_bytes <= stats->ram_total_bytes);
}
