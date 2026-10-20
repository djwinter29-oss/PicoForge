/** @file
 * @brief Per-core FreeRTOS runtime and SDK-linked SRAM reservation telemetry.
 */
#ifndef PF_RUNTIME_STATS_H
#define PF_RUNTIME_STATS_H

#include <stdint.h>

#define PF_RUNTIME_CORE_COUNT 2u

/** Per-core CPU interval estimates and static SRAM reservation; no live heap or stack occupancy. */
typedef struct {
    uint16_t cpu_usage_basis_points[PF_RUNTIME_CORE_COUNT]; /**< Per-core non-idle fraction, 0..10000; UINT16_MAX until sampled. */
    uint32_t ram_used_bytes; /**< Linked main/scratch data plus reserved core stacks; task stacks are in data. */
    uint32_t ram_total_bytes; /**< SDK-selected on-chip SRAM capacity, excluding external RAM. */
} pf_runtime_stats_t;

/** @brief Sample from task context; serialized internally and cached for intervals below 250 ms.
 * @param stats Non-NULL caller-owned snapshot destination.
 */
void pf_runtime_stats_read(pf_runtime_stats_t *stats);

#endif