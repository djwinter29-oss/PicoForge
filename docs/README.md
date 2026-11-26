# Reading the Documentation

Use the [project README](../README.md) for the capability overview and top-level links. This page organizes documents
by reading order and topic ownership. Start with [getting started](guides/getting-started.md); do not begin with old
measurement records.

## Reading Order

1. **Use the device:** [hardware](guides/hardware.md), your dashboard's guide, then the
   [console](guides/console.md), [wireless](guides/wireless.md), or
   [storage benchmark](guides/sdcard-benchmark.md) procedure you need.
2. **Understand the foundation:** [project scope](../README.md#project-scope), [architecture](reference/architecture.md),
   then the relevant reference. These describe implemented behavior, not a feature roadmap.
3. **Maintain and validate:** [firmware testing](development/firmware-testing.md) or
   [host development](development/host-tools.md), followed by [qualification gaps](validation/qualification-gaps.md).
4. **Publish:** use the [release procedure](deployment/release.md). Historical passes do not qualify a new revision.

## Document Ownership

Keep each topic in one authoritative document; other guides link to it rather than copy its tables or procedures.

| Topic | Authoritative location |
| --- | --- |
| Wiring, electrical limits, board and breakout connections | [Hardware guide](guides/hardware.md) |
| Runtime, storage ownership, automount and worker admission | [Architecture](reference/architecture.md) |
| Clock profiles, flash settings and forced-save safety | [Settings](reference/settings.md) |
| USB management report layouts and command semantics | [USB HID reference](reference/usb-hid.md) |
| Radio execution, credential extension, provisioning and TCP CLI authorization | [Wireless integration](development/wireless-integration.md) |
| BLE advertising, GATT layout, authorization and command form | [Bluetooth LE protocol](reference/bluetooth.md) |
| SD protocol, block interfaces and recovery | [SD driver](reference/sdcard/driver.md) |
| PIO/SPI engine lifecycle and wire timing | [Physical links](reference/sdcard/physical-links.md) |
| CLI commands and terminal use | [Console guide](guides/console.md) |
| CLI dependency, registration and transport internals | [Console integration](development/console-integration.md) |
| Benchmark safety, timing boundaries and interpretation | [Benchmark guide](guides/sdcard-benchmark.md) |
| Direct dashboard independence and host checks | [Host development](development/host-tools.md) |
| Build/test commands and hardware acceptance | [Firmware testing](development/firmware-testing.md) |
| Build options, optional modules and configuration-specific checks | [Build configuration](reference/build-configuration.md) |
| GitHub Actions release packaging, publication and download verification | [Release and publishing](deployment/release.md) |
| Current evidence gaps and SD review dispositions | [Qualification gaps](validation/qualification-gaps.md), [SD review](validation/sd-review.md) |
| Dated observations, measurements and superseded reviews | [Records](validation/records/README.md) |

`guides/` contains operating procedures, `reference/` implementation contracts, `development/` maintenance workflows,
`deployment/` release and publishing procedures, and `validation/` evidence and its limits.
Records retain their original configuration and may mention retired
commands or build options; use current guides to operate the current firmware.

## Layout and Naming

- Keep the root README as the overview and top-level index; this page owns reading paths and document ownership.
- Name current pages by topic or task using lowercase hyphenated filenames. Keep component READMEs beside their code.
- Use subdirectories only for cohesive multi-page topics, such as the SD driver. Do not mirror every source directory.
- Keep test commands with their owning development guide; the test READMEs own suite placement and fixture rules.
- Name dated evidence `YYYY-MM-DD-topic.md`. If its date is unknown, say so rather than inventing one.
- Record revision, configuration, result and evidence availability in historical records. Keep current procedures
  and open qualification status outside those records.
- When behavior changes, update its authoritative page and link to it from other audiences' guides. Repeat short
  safety warnings at the point of use, not full protocol tables or state machines.
