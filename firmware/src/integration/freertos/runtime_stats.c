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

static uint64_t previous_time; /**< Last interval boundary in microseconds. */
static uint64_t previous_idle; /**< Cumulative idle time at the last boundary. */
static bool sampled; /**< Whether an interval baseline exists. */
static uint16_t cpu_usage = UINT16_MAX; /**< Cached interval result; unavailable until first interval. */

void pf_runtime_stats_read(pf_runtime_stats_t *stats)
{
    configASSERT(stats != NULL);
    taskENTER_CRITICAL();
    uint64_t now = time_us_64();
    uint64_t idle = ulTaskGetIdleRunTimeCounter();
    if (sampled && now - previous_time >= 250000u) {
        uint64_t capacity = (now - previous_time) * configNUMBER_OF_CORES;
        uint64_t idle_elapsed = idle - previous_idle;
        /* Wall and kernel idle timestamps are adjacent, not simultaneous; saturate sampling skew at idle. */
        cpu_usage = idle_elapsed >= capacity ? 0u :
            (uint16_t)(((capacity - idle_elapsed) * 10000u) / capacity);
    }
    if (!sampled || now - previous_time >= 250000u) {
        previous_time = now;
        previous_idle = idle;
        sampled = true;
    }
    stats->cpu_usage_basis_points = cpu_usage;
    taskEXIT_CRITICAL();
    stats->ram_total_bytes = SRAM_END - SRAM_BASE;
    stats->ram_used_bytes = (uintptr_t)__end__ - SRAM_BASE +
        ((uintptr_t)__scratch_x_end__ - (uintptr_t)__scratch_x_start__) +
        ((uintptr_t)__scratch_y_end__ - (uintptr_t)__scratch_y_start__) +
        ((uintptr_t)__StackTop - (uintptr_t)__StackBottom) +
        ((uintptr_t)__StackOneTop - (uintptr_t)__StackOneBottom);
    configASSERT(stats->ram_used_bytes <= stats->ram_total_bytes);
}
