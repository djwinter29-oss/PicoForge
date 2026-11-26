# SD Review Dispositions

This page tracks unresolved evidence needs and retained design constraints, not a hardware pass.
The [historical remediation record](records/sd-review-remediation.md) preserves the reviews based on `cc2816d`
and `8419b1f`; their working-diff test counts and instrumentation statements do not describe the current revision.
Current implementation is defined by the [SD driver](../reference/sdcard/driver.md) and
[storage lifecycle](../reference/architecture.md#storage-ownership-lifecycle).

## Open Evidence Needs

| Area | Current disposition | Evidence needed |
| --- | --- | --- |
| USB service latency | MSC media callbacks remain synchronous. IRQ waits alone do not dispatch HID/CDC/BOT on the USB task. | Agree a service deadline, then measure callback/lock p95, p99, worst case and HID/CDC service gaps. |
| End-to-end MSC behavior | Callback tests and pinned-source contracts are not execution of TinyUSB's full BOT state machine. | Observe short transfers, reset/abort, CSW status/residue, sense, sync and eject with actual card completion. |
| Stack headroom | Periodic self-task watermark logging is implemented; unused fill is not certified margin. | Record USB/worker minima under representative concurrent workloads; see [stack observations](../reference/architecture.md#stack-observations). |
| SDIO optimizations | Fused copy and IRQ waits remain opt-in. The historical CMD25 persistence gain does not select either option. | Matched four-cell target measurements using the [controlled comparison procedure](../reference/sdcard/driver.md#performance-controls-and-status). |
| Larger MSC staging or asynchronous I/O | Not implemented; no throughput gain or safe error-delivery boundary is established. | Quantified benefit, SRAM/request lifetime budget, and BOT/reset/error delivery before CSW. |
| Durability and compatibility | Short filesystem benchmarks do not qualify power loss, endurance or all host/card combinations. | Configuration-specific board results following [hardware acceptance](../development/firmware-testing.md#hardware-validation). |

## Retained Constraints

- FatFs executes only on the storage worker. Per-block recursive locking does not serialize a filesystem transaction.
- Ownership handoff requires checked close/sync and unmount; fault cleanup must not claim a clean flush.
- Failed or uncertain payload writes are never automatically replayed.
- Current TinyUSB endpoint staging is 512 bytes. Larger callback inputs in tests do not prove end-to-end batching.

For detailed contracts, use the references above rather than historical review prose.
Record new measurements with the exact source revision, artifact hashes, board/card/host configuration, method and
pass/fail/not-run results. Keep unavailable evidence explicitly unavailable; host tests cannot close target-only gaps.
