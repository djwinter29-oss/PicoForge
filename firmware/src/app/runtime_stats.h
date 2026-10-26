/** @file
 * @brief Per-core FreeRTOS runtime and SDK-linked SRAM reservation telemetry.
 */
#ifndef PF_RUNTIME_STATS_H
#define PF_RUNTIME_STATS_H

#include <stdint.h>

/** Number of cores reported; RP2040 and RP2350 both have two. */
#define PF_RUNTIME_CORE_COUNT 2u

/** Value reported for a core's CPU usage until one full sampling interval has elapsed. */
#define PF_RUNTIME_CPU_UNAVAILABLE UINT16_MAX

/** Per-core CPU interval estimates and static SRAM reservation; no live heap or stack occupancy. */
typedef struct {
    /** Per-core non-idle fraction, 0..10000; PF_RUNTIME_CPU_UNAVAILABLE until sampled. */
    uint16_t cpu_usage_basis_points[PF_RUNTIME_CORE_COUNT];
    uint32_t ram_used_bytes; /**< Linked main/scratch data plus reserved core stacks; task stacks are in data. */
    uint32_t ram_total_bytes; /**< SDK-selected on-chip SRAM capacity, excluding external RAM. */
} pf_runtime_stats_t;

/** @brief Sample from task context; serialized internally and cached for intervals below 250 ms.
 * @param stats Non-NULL caller-owned snapshot destination.
 */
void pf_runtime_stats_read(pf_runtime_stats_t *stats);

#endif
