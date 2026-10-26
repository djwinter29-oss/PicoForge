# Qualification Gaps

PicoForge is under active development. A successful build or short test does not qualify sustained SD-card
reliability, USB mass-storage compatibility, or power-loss recovery. Back up important card data before use and
follow the [getting-started safety precautions](../guides/getting-started.md#before-you-connect).

## Areas Not Fully Qualified

- **Sustained SD-card reliability:** existing measurements apply to specific firmware, hardware, cards, and test
  durations. They do not establish reliability across other configurations or sustained workloads.
- **Physical USB mass-storage compatibility:** host tests and source contracts do not establish end-to-end USB
  behavior across host operating systems, including eject, disconnect, reset, and error recovery.
- **Power-loss recovery:** existing validation does not establish filesystem integrity or data durability after
  interrupted writes or unexpected power loss.

These are evidence gaps, not a claim that every listed scenario fails.

## Evidence and Follow-up

- [Test and review records](records/README.md): dated measurements and reviews with configuration-specific scope.
- [SD remediation matrix](sd-design-remediation.md): detailed review dispositions and outstanding measurement needs.
- [Firmware development and testing](../development/firmware-testing.md): validation procedures and hardware limits.

Keep detailed findings and test results in those authoritative documents rather than duplicating them here.
