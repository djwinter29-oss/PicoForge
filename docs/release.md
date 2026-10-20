# Release packages and prerequisites

After review and software validation, tag the intended commit as `vX.Y.Z` and push that tag through the normal
publication process. Annotated (`git tag -a v1.2.3 -m 'Release 1.2.3'`) and lightweight (`git tag v1.2.3`) tags
are supported. Each component must be decimal 0–255, without leading zeros except `0`. Prerelease suffixes are
not accepted. GitHub's tag glob is broad; an early validation job rejects anything outside this exact format
before any build runs. Manual dispatch must select a tag with the same format; branch dispatch fails closed.
The stripped `X.Y.Z` supplies `PF_FIRMWARE_VERSION` to every build, including HID identity and SDK program metadata.
Local developer builds retain `0.0.0-dev` unless overridden.

The workflow produces four checksum-bearing artifacts for every combination of:

- Board: `pico` and `pico2`.
- SD transport: `SDIO_PIO` and `SPI`.

All artifacts are FreeRTOS applications using the SDK stock clock. Artifact names are
`firmware-vX.Y.Z-BOARD-TRANSPORT`, with `-HS` for SDIO builds. Packages contain ELF, UF2, and `SHA256SUMS`; the
workflow uploads Actions artifacts and does not publish a GitHub release automatically.

Packaging is gated on the reusable host-validation workflow: Ceedling firmware tests, Python host tests on
3.10 and 3.12, and WebHID and .NET host tests. Each PR/release matrix entry performs one firmware build; runtime
clock selection remains a boot setting and does not multiply CI artifacts.

Release builds set `SOURCE_DATE_EPOCH` to the checked-out commit's committer timestamp before configuring CMake.
The UTC date in image filenames therefore stays fixed when rebuilding the same commit, including through annotated
tags; local builds can set the same variable explicitly. Without it, local filenames use the configuration date.
This stabilizes the date input, not a guarantee of bit-for-bit reproducibility across toolchains or build paths.

The SDK defaults are 125 MHz on RP2040 and 150 MHz on RP2350. The dashboard can apply supported runtime clocks;
150 MHz is an overclock on RP2040, and 250 MHz is an overclock on both MCUs. Accepted SDK clock configuration
does not establish hardware safety.
SDIO High-Speed negotiation defaults to ON for new builds and is explicitly ON in release builds. It requests the
highest supported safe planned rate up to 50 MHz, clamped by the PIO ceiling of SYSCLK/4: at most 37.5 MHz at
150 MHz CPU, and up to 50 MHz at 250 MHz CPU. Unsupported High Speed or failed faster-bus validation retains the
bounded 25 MHz safe fallback policy; failed fallback aborts initialization. Actual rates depend on card support and
divider planning. SPI follows its separate card-rate and divider policy. See [SD design](sdcard-design.md).

Product release remains blocked on allocation/owner approval of a USB identity. `0xCAFE:0x4011` is the documented
development VID/PID and remains unchanged; it must not be represented as an allocated product identity.
Board/card/wiring and clock combinations also require hardware qualification before product shipment. Software CI
and historical lab records do not qualify every release configuration. No hardware validation is claimed here.
