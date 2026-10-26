# PicoForge Project Vision

For the project target, current capabilities, and documentation index, see the [README](../../README.md).
This document describes intended development direction, not a claim that every capability is available or scheduled.

## Developer Experience

**Write host code. Control real hardware. Let PicoForge handle the embedded infrastructure.**

Pico-family boards are a low-cost starting point for experiments. PicoForge aims to reduce the repeated toolchain,
firmware, communication, and host-integration work between connecting a peripheral and using its data.
The dashboard is the interactive companion for board inspection, configuration, and diagnostics.

| Path | Intended experience |
| --- | --- |
| **Use it as-is** | Flash a supported build, connect a peripheral, and use a Python or C# host library to configure it and exchange data. |
| **Build on it** | Select a supported set of capabilities and add application-specific firmware and host behavior. |

Today, these paths start with board-management and SD-storage workflows. Peripheral bridges, public peripheral APIs,
and broader build selection still need implementation.

## Capabilities That Enable the Vision

| Capability | Purpose | Current scope |
| --- | --- | --- |
| **SD storage** | Retain data locally and retrieve it through managed storage workflows. | Implemented; qualification limits are documented separately. |
| **One I²C master and one SPI master** | Configure and communicate with peripherals from host code. | Planned host-accessible bridges. |
| **PWM control** | Generate device-side, hardware-timed signals configured by the host. | Candidate extension. |
| **Python and C# host libraries** | Provide familiar bus operations, board discovery, configuration, and meaningful errors. | Host applications exist; public peripheral APIs are planned. |
| **Selectable builds** | Keep only the capabilities needed for a specialized project. | SD transport selection exists; broader capability selection is planned. |
| **Wireless access** | Reach the same device capabilities without a USB cable on wireless boards. | Future option; Wi-Fi and Bluetooth scope is not committed. |

SD storage is the first foundation: onboard flash has limited capacity and write endurance, and immediate host
streaming is constrained by USB Full Speed or wireless throughput. Local storage can separate recording from later
retrieval. High-rate recording still requires a qualified acquisition pipeline, buffering, and handling of card
write stalls; storage support alone is not a logic analyzer.

## Scope Boundaries

- Bus access does not automatically supply a sensor driver. Developers still need sensor-specific logic or a
  compatible host library; existing Python/.NET library compatibility depends on the interfaces those libraries expect.
- Complete bus transactions and PWM timing belong on the Pico. Desktop scheduling is not a real-time signal source.
- New capabilities must coexist with storage and respect pin, peripheral, and memory constraints. Selectable builds
  should expose supported combinations, not promise arbitrary combinations.
- Future recording, diagnostics, and automation should extend the same firmware-and-host system, not introduce
  disconnected demonstrations. Wireless should be another access option, not a separate programming model.

## Showing Progress

Use the [demonstration plan](../development/demonstrations.md) to show concrete developer workflows.
Keep reliability and compatibility evidence separate in the [qualification gaps](../tests/qualification-gaps.md)
and linked test records. Implementation boundaries live in the [architecture](architecture.md).
