# Release packages and prerequisites

After review and software validation, tag the intended commit as `vX.Y.Z` and push that tag through the normal
publication process. Annotated (`git tag -a v1.2.3 -m 'Release 1.2.3'`) and lightweight (`git tag v1.2.3`) tags
are supported. Each component must be decimal 0–255, without leading zeros except `0`. Prerelease suffixes are
not accepted. GitHub's tag glob is broad; an early validation job rejects anything outside this exact format
before any build runs. Manual dispatch must select a tag with the same format; branch dispatch fails closed.
The stripped `X.Y.Z` supplies `PF_FIRMWARE_VERSION` to every build, including HID identity and SDK program metadata.
Local developer builds retain `0.0.0-dev` unless overridden.

The workflow produces separate checksum-bearing artifacts for every combination of:

- Board: `pico` and `pico2`.
- CPU clock: 150 and 250 MHz.
- SD transport: `SDIO_PIO` and `SPI`.
- Port: `FREERTOS` application and `BAREMETAL` driver smoke image.

Artifact names are `firmware-vX.Y.Z-BOARD-cpuCLOCK-TRANSPORT-PORT`, with `-HS` for SDIO builds.
Bare-metal images are driver checks, not the USB application; their artifact validation gate runs before packaging.
FreeRTOS images run the applicable application ELF and contract tests. Packages contain ELF, UF2, and `SHA256SUMS`;
the workflow uploads Actions artifacts and does not publish a GitHub release automatically.

The SDK defaults are 125 MHz on RP2040 and 150 MHz on RP2350. Release clocks are explicit: 150 MHz is an overclock
on RP2040; 250 MHz is an overclock on both MCUs. Accepted SDK clock configuration does not establish hardware safety.
SDIO High-Speed negotiation defaults to ON for new builds and is explicitly ON in release builds. It requests the
highest supported safe planned rate up to 50 MHz, clamped by the PIO ceiling of SYSCLK/4: at most 37.5 MHz at
150 MHz CPU, and up to 50 MHz at 250 MHz CPU. Unsupported High Speed or failed faster-bus validation retains the
bounded 25 MHz safe fallback policy; failed fallback aborts initialization. Actual rates depend on card support and
divider planning. SPI follows its separate card-rate and divider policy. See [SD design](sdcard-design.md).

Product release remains blocked on allocation/owner approval of a USB identity. `0xCAFE:0x4011` is the documented
development VID/PID and remains unchanged; it must not be represented as an allocated product identity.
Board/card/wiring and clock combinations also require hardware qualification before product shipment. Software CI
and historical lab records do not qualify every release configuration. No hardware validation is claimed here.
