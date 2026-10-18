/** @file
 * @brief Single-core FreeRTOS runtime and SDK-linked SRAM reservation telemetry.
 */
#ifndef PF_RUNTIME_STATS_H
#define PF_RUNTIME_STATS_H

#include <stdint.h>

/** CPU interval estimate and static SRAM reservation; no live heap or stack occupancy. */
typedef struct {
    uint16_t cpu_usage_basis_points; /**< Non-idle time, 0..10000; UINT16_MAX before the first interval. */
    uint32_t ram_used_bytes; /**< Linked main/scratch data plus reserved core stacks; task stacks are in data. */
    uint32_t ram_total_bytes; /**< SDK-selected on-chip SRAM capacity, excluding external RAM. */
} pf_runtime_stats_t;

/** @brief Sample from task context; serialized internally and cached for intervals below 250 ms.
 * @param stats Non-NULL caller-owned snapshot destination.
 */
void pf_runtime_stats_read(pf_runtime_stats_t *stats);

#endif
