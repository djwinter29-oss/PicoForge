# Reading the Documentation

The [project README](../README.md#documentation) is the documentation index and capability overview.
Start with [getting started](guides/getting-started.md); do not begin with old measurement records.

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
| Radio execution, credential extension, provisioning and BLE authorization | [Wireless reference](reference/wireless.md) |
| SD protocol, block interfaces and recovery | [SD driver](reference/sdcard/driver.md) |
| PIO/SPI engine lifecycle and wire timing | [Physical links](reference/sdcard/physical-links.md) |
| CLI commands and terminal use | [Console guide](guides/console.md) |
| CLI dependency, registration and transport internals | [Console integration](development/console-integration.md) |
| Benchmark safety, timing boundaries and interpretation | [Benchmark guide](guides/sdcard-benchmark.md) |
| Dashboard component isolation and host checks | [Host development](development/host-tools.md) |
| Build/test commands and hardware acceptance | [Firmware testing](development/firmware-testing.md) |
| GitHub Actions release packaging, publication and download verification | [Release and publishing](deployment/release.md) |
| Current evidence gaps and SD review dispositions | [Qualification gaps](validation/qualification-gaps.md), [SD review](validation/sd-review.md) |
| Dated observations, measurements and superseded reviews | [Records](validation/records/README.md) |

`guides/` contains operating procedures, `reference/` implementation contracts, `development/` maintenance workflows,
`deployment/` release and publishing procedures, and `validation/` evidence and its limits.
Records retain their original configuration and may mention retired
commands or build options; use current guides to operate the current firmware.
