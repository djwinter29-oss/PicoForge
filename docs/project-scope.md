# PicoForge Project Scope

For implemented capabilities and the documentation index, see the [README](../README.md).
This document defines the maintained project boundary, not a feature roadmap or a claim of hardware qualification.

## Maintained Foundation

PicoForge is a reusable firmware-and-host foundation for Raspberry Pi Pico-family products.
Its scope is the existing board-management, diagnostics, USB communication, persistent configuration, SD storage,
and optional W-board wireless integration described in the implementation guides.

Feature expansion is closed. Work in this project focuses on bug fixes, reliability, compatibility, documentation,
and qualification of existing behavior. New product capabilities belong in downstream projects; there is no
peripheral-bridge, acquisition, or general-purpose framework roadmap.

## Foundation Versus Product

| Responsibility | Owner |
| --- | --- |
| Existing board management, diagnostics, persistence, communication, and storage | PicoForge, within documented interfaces and limits. |
| Application-specific hardware behavior, processing, commands, and host UI | Downstream product. |
| Maintenance and qualification of existing foundation services | PicoForge. |

Python and .NET may share their own frontend. Direct must not share dashboard components with them; see the
[dashboard component isolation policy](development/host-tools.md#dashboard-component-isolation).
Using the same firmware protocol specification does not require sharing frontend implementations.

## Reusing the Baseline

1. Pin a tested source revision and the dependencies it selects.
2. Identify which existing services the product needs and which interfere with its resource or timing budget.
3. Define ownership of GPIOs, PIO state machines, DMA channels, IRQs, memory, and peripheral instances.
4. Implement and test product-specific behavior in the downstream project.
5. Report foundation defects with a focused reproduction and the affected configuration.

Pinning a revision gives a reproducible starting point, not compatibility with every workload. SD transport is selected
at build time. The FreeRTOS SMP runtime supports application tasks alongside platform services, but neither core is
permanently reserved for an application. SMP alone provides no dedicated-core isolation or hard real-time guarantee.
See the [runtime policy](reference/architecture.md#core-and-runtime-policy) and
[layer responsibilities](reference/architecture.md#layer-responsibilities) before assigning resources.

## Maintenance and Evidence

Preserve existing public contracts and safety gates when fixing defects. Keep changes focused, add the smallest
relevant regression check, and document any necessary compatibility change.
Software checks establish specific contracts, not physical reliability. Hardware evidence must identify the source
revision, board, host environment, wiring, and workload.

Keep implemented wireless scope in the [wireless guide](guides/wireless.md), outstanding evidence gaps in
[qualification gaps](validation/qualification-gaps.md), observed behavior in
[demonstration records](validation/records/dashboard-capture.md) and linked test records, and publication procedure in the
[release guide](release.md).
