# PicoForge Project Vision

For current capabilities and the documentation index, see the [README](../../README.md).
This document describes intended development direction, not a claim that every capability is available or scheduled.

## Project Target

PicoForge is an integrated firmware-and-host platform that turns Raspberry Pi Pico-family boards into programmable
interfaces between hardware and host applications. It helps developers evaluate sensors, control peripherals, and
automate experiments from Python or C# without writing custom embedded firmware for supported operations. Local
storage, device management, and a customizable firmware foundation support the transition from an initial experiment
to a specialized embedded application.

## Audience and Value

The primary audience is developers evaluating sensors and peripherals, automating experiments, or prototyping
hardware-connected applications from Python or C#. Host-side hardware development is the central purpose;
board management, diagnostics, and storage support that workflow.

- **Start experimenting sooner:** reduce repeated embedded setup and host-integration work for supported operations.
- **Keep timing and recording on the device:** execute hardware transactions locally and retain data when immediate
  host streaming is unsuitable. Peripheral execution and recording pipelines still need implementation.
- **Build beyond the prototype:** customize the same firmware-and-host foundation for application-specific behavior.

Low-cost Pico hardware supports this value, but reducing developer effort is the primary benefit.

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

## Lead Use Case: I²C Sensor Evaluation

The first reference workflow targets a developer who wants to evaluate one I²C sensor from a computer without
creating custom embedded firmware. The sensor model and supported wiring remain to be selected.

The intended experience is:

1. Flash a supported firmware build and connect the documented sensor.
2. Install the host package and discover the board and its I²C capability.
3. Run a Python example to configure the sensor and obtain interpreted measurements.
4. Run an equivalent C# example using the same settings and measurement interpretation.
5. Diagnose a missing sensor, transaction failure, or disconnected board through explicit errors and relevant
   companion diagnostics.

Sensor-specific register logic belongs in the example or a compatible host driver, not the generic firmware.
This workflow needs no SD recording, PWM, wireless, or custom application task.
SPI remains another planned bus, not a dependency of the first I²C example.

The example should publish reproducible setup steps, environment details, and observed first-reading setup time,
without promising an unmeasured time target. Successful evaluation is a complete outcome; recording and customization
are optional growth paths, not mandatory stages.

## Differentiation Through Workflow

The intended distinction is continuity from host-side exploration to device-side execution and customization,
not an assertion that PicoForge uniquely provides bus bridges or dashboards.

**Start as a host-controlled tool. Grow into your own embedded application.**

### What Should Carry Forward

| Design commitment | Developer value | How to demonstrate it |
| --- | --- | --- |
| **One device contract across host tools** | Switching between Python, C#, and companion diagnostics should not require relearning supported operations or error meanings. | Run equivalent sensor operations and failure cases in Python/C#; distinguish bus-level failures from sensor interpretation errors. |
| **Retained services during customization** | Add application behavior without rebuilding every discovery, configuration, communication, and storage component. | Document the components retained, modified, and replaced in a task-based extension; do not promise unchanged APIs for new application behavior. |
| **Local execution when justified** | A defined acquisition can continue without a host request for every sample, with explicit limits. | Record locally, inspect completion or overflow, and retrieve the result using a documented configuration. |

These are intended product commitments to verify, not additional generic frameworks to build. The first commitment
belongs to the lead I²C workflow; the others are optional follow-on demonstrations. Retain services only where
compatible with the application's resource and timing requirements, rather than forcing every extension to keep
the entire platform.

The existing dual-core FreeRTOS SMP runtime is part of that foundation. Developers can add application tasks alongside
platform services instead of replacing the firmware with a separate acquisition program. Both cores remain under
one scheduler; neither is permanently reserved for the application or the platform. Extending the system still
requires explicit peripheral ownership, serialization, and resource budgeting. SMP alone provides neither isolation
nor a hard real-time guarantee. See [runtime policy](architecture.md#core-and-runtime-policy) for the implementation.

| Stage | Intended outcome | Evidence needed |
| --- | --- | --- |
| **Explore from the host** | Evaluate the reference I²C sensor and understand failures through host code and companion diagnostics. | The lead workflow above, with reproducible setup and matching Python/C# behavior. |
| **Record locally** | Configure a bounded acquisition, execute it on the Pico, record to SD, and retrieve results for analysis. | A defined workload with measured buffering limits, recording throughput, and explicit overflow behavior. |
| **Extend the device** | Add application tasks to the shared SMP runtime while retaining useful communication, management, and storage services. | A tested task-based extension documenting resource ownership, shared-service use, and what must change. |

Peripheral access and acquisition remain planned. Recording is not simply a sequence of host requests: device-side
execution should avoid a USB round trip for each sample. Define a concrete workload before choosing scheduling,
buffering, or command interfaces; do not introduce an arbitrary scripting engine to cover hypothetical applications.

Validate the value with developer feedback and comparisons against relevant alternatives before claiming uniqueness
or superiority. A broader interface list alone does not establish differentiation.

Compare the same sensor-evaluation task rather than interface counts: required toolchains, firmware changes,
host setup, error diagnosis, and steps needed for later device-side customization. Record trade-offs as well as
advantages; no comparative performance or usability claim is established by this vision.

Use two comparisons: sensor evaluation alone, and evaluation followed by one custom device task. This separates the
value of the ready-to-use bridge from the value of the extension path. Include developer effort, dependencies,
resource overhead, and limitations, not just successful steps. If a simpler tool meets the task better, state that
trade-off rather than expanding PicoForge's scope to win every comparison.

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

### Platform Versus Application

| Responsibility | Scope |
| --- | --- |
| **Reusable platform** | The shared SMP runtime, board management, communication, storage, and planned bounded peripheral operations with host APIs. |
| **Application or example** | Sensor register logic, interpretation of measurements, custom processing, and workload-specific acquisition tasks. |
| **Optional future extensions** | PWM, wireless, general tracing, and broader build selection, each requiring its own bounded use case before expansion. |

These boundaries define responsibility, not a new delivery schedule. Existing management and SD workflows remain
useful independently; host peripheral access does not depend on first delivering recording or wireless.

### Feature Admission

Before adding a capability to the reusable platform, define:

1. The specific developer task it improves and why an application example is insufficient.
2. Its bounded operations, capability discovery, error reporting, and host behavior.
3. Pin/peripheral ownership, memory needs, concurrency, and coexistence with existing services.
4. A runnable check or example and the hardware evidence needed to substantiate its limits.

Start with a concrete supported configuration. Add build options only for demonstrated selection needs, rather than
creating a general plugin system, scripting engine, or arbitrary combination matrix.

### Explicit Limits

- PicoForge does not eliminate hardware knowledge, guarantee compatibility with every sensor library, or replace
  real-time embedded programming and specialized test instruments.
- Bus access does not automatically supply a sensor driver. Developers still need sensor-specific logic or a
  compatible host library; existing Python/.NET library compatibility depends on the interfaces those libraries expect.
- Complete bus transactions and PWM timing belong on the Pico. Desktop scheduling is not a real-time signal source.
- New capabilities must coexist with storage and respect pin, peripheral, and memory constraints. Selectable builds
  should expose supported combinations, not promise arbitrary combinations.
- Future recording, diagnostics, and automation should extend the same firmware-and-host system, not introduce
  disconnected demonstrations. Wireless should be another access option, not a separate programming model.

For each proposed feature, ask: does it shorten the path from connected hardware to a useful application, or support
that application as it grows? Features that do neither are outside the primary direction.

## Showing Progress

Use the [demonstration plan](../development/demonstrations.md) to show concrete developer workflows.
Keep reliability and compatibility evidence separate in the [qualification gaps](../tests/qualification-gaps.md)
and linked test records. Implementation boundaries live in the [architecture](architecture.md).
