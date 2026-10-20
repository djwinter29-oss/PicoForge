# Static review remediation validation

Historical record, not current validation. The table and findings below describe the checks run on the working-tree
edits made on top of `bde8d7e` before they were committed as `ac4ae7a`; the former "edits remain uncommitted"
statement was true only at that time. Later changes (for example the DeviceMonitor command-write locking and the
`tools/firmware/load.sh` Tcl quoting) are not covered by these results. For current procedures and commands, see the
[test guide](../../tests/README.md). Software-only checks, including cross-compiled target builds; no hardware
qualification claimed.

| Command | Final outcome |
|---|---|
| `PATH="$PWD/build/coverage-tools/bin:$PATH" "$(ruby -rrubygems -e 'print Gem.user_dir')/bin/ceedling" test:usb_hid_sd_responsiveness test:storage_worker_automount` | Exit 0; 7 USB/HID tests and 3 automount tests passed. |
| `PYTHONPATH=host/python/src /usr/bin/python3 -m pytest tests/contracts/test_workflows.py host/python/tests/test_protocol.py -q` | Exit 0; 19 passed, 36 subtests passed. Includes Ruby YAML parsing of both build workflows. |
| `node --test host/webhid/tests/protocol.test.mjs` | Exit 0; 20 tests passed. |
| `dotnet build host/dotnet/PicoForge.Tests --no-restore --verbosity quiet` | Exit 0; zero warnings/errors. |
| `dotnet test host/dotnet/PicoForge.Tests --no-restore --verbosity normal` | Exit 0; 20 passed. |
| `sh -n tools/firmware/build.sh` | Exit 0. |
| `ruby -ryaml -e 'ARGV.each { |f| YAML.load_file(f) }' .github/workflows/pr-check.yml .github/workflows/release.yml` | Exit 0; both YAML files parsed. |
| `tools/firmware/build.sh` across both boards, SPI/SDIO, FreeRTOS/bare-metal (six CI configurations) | Exit 0 for all six; four firmware application builds and two bare-metal smoke builds linked. No target was flashed or run. |
| `PYTHONPATH=host/python/src /usr/bin/python3 -m pytest tests/contracts tests/test_validation_integrity.py -q` | Exit 0; 90 passed. |
| ELF/UF2 output checks across the six `build/review-*` directories | Each directory contains one ELF and one UF2. |
| `git diff --check` | Exit 0. |

The required wrapper `tools/validation/setup-ceedling-testing.sh test:usb_hid_sd_responsiveness
 test:storage_worker_automount` exited 2 trying to acquire the coverage environment's uv lock in the read-only
user cache. No installation was performed. The table uses the already installed pinned Ceedling 1.1.10 directly
with the existing coverage tools on PATH. Default `python3 -m pytest` lacked pytest; `/usr/bin/python3` has it.
A combined pytest invocation initially omitted PYTHONPATH and failed collection; the corrected command above passed.
Initial C fixture linkage/assertion errors and an xUnit blocking-operation analyzer error were corrected before
worker validation. VSTest was blocked by the worker sandbox's denied local socket; independent coordinator
validation subsequently ran all 20 .NET tests successfully without changing sandbox settings.

Finding 1: USB's five-second no-progress state cancels the matching session and polls replies independently of
CDC output space. The full-queue regression exercises the worker's actual send loop, cancellation, delayed done
publication, admission release, and disconnect cleanup. Completion is simulated by the queue fake after producer
cancellation; this is not a scheduler or FatFs end-to-end test. Existing file producer loops close opened files
when a reply returns false; mutation commands retain their own close/sync behavior. Worker completion remains
queued after execution/cleanup and admission remains held until done is drained. The explicit CLI truncation
notice and DTR reopen requirement are documented in [CDC CLI](../cdc-cli.md#stalled-output).

Finding 2: DeviceMonitor reads under a device-handle lock and publishes under the snapshot lock. Commands,
handle publication and disposal acquire the device lock first; disposal releases the snapshot lock before I/O.
Disposal waits for an in-flight handle operation before closing the client.
Snapshot acquires only its own lock. The focused lock test passed in the .NET suite. It proves snapshot reads do
not wait on the handle-operation lock, but uses a locked test object rather than an injected blocking HID read;
the concrete sealed HID client has no injectable read seam, and no hardware was accessed.

Finding 3: Shared filesystem report vectors cover valid formats and malformed version/length/padding. All three
host decoders consume them. C checks the encoder against the exFAT vector: firmware does not parse incoming
filesystem telemetry, so host rejection vectors have no equivalent firmware parser entry point.

Finding 4: Six build configurations cover both boards and all supported transport/port pairs; MSC checks are
FreeRTOS-only. Manual packaging retains development version metadata and uploads workflow artifacts only.
Workflow contracts and six local cross-compilation configurations passed; remote runner execution remains
unverified. Build commands used `--build-dir build/review-BOARD-TRANSPORT-PORT` for each corresponding matrix row.

Finding 5: The boot-only enumeration wait is preserved with a source `ponytail:` rationale.

Finding 6: Both tasks periodically log their own FreeRTOS minimum untouched stack in words. Configuration/source
contracts pass. Sampling is observational and does not certify margin; no target measurements were taken.
See [architecture validation limits](../architecture.md#review-remediation-software-checks) for remaining MSC,
power-loss, dual-core stress and stack qualification gaps.

## Follow-up remediation (software checks only)

Not a hardware result; the sections above remain historical.

* Input Report 7 (filesystem format) is now documented in [USB protocol](../usb-protocol.md#input-report-7).
  `test_usb_protocol_documents_filesystem_input_report` was RED (no section), then GREEN.
* `release.yml` now runs the BAREMETAL `tests/artifacts/test_sd_ports.py` check between build and package, as
  `pr-check.yml` does. `test_workflows_validate_baremetal_artifacts_after_build` is parameterized over both
  workflows and checks release ordering; it was RED for `release.yml`, then GREEN.
* `tests/pio/test_sdio_pio_timing.py` models the assembled `data_tx_fast` opcodes in system cycles: TX SM at half the
  clock divider (floor and ceiling tick placement, dividers 2.0-3.0 including fractional 2.5), launch phase and
  cadence, setup/hold of at least one cycle, status token bits (accepted/CRC/write error), card gap, and busy
  completion, with 0/1 cycle input latency. Single-opcode mutants (launch phase, cadence, status edge, bit count, busy
  wait) are asserted to fail. Production PIO is unchanged: no check failed within the bounds below.
  Boundaries (HIL only): FIFO/DMA starvation, pad skew, real card output delay and sustained transfers.
  Card output delay is modelled as 1-2 cycles after the rising edge; 0 fails at divider 2.75 with one cycle
  of input latency (the sample lands on the next bit) and 3 or more fails at 50 MHz. Divider 2.75 needs about
  275 MHz and is not hardware-qualified (only 200 MHz / 50 MHz is).
* The CMD18 rejection/CMD12 cleanup concern at `sd_native_host.c` is not confirmed. `test_sd_native_host_read_batch.c`
  adds coverage that already passed on the baseline (not a RED fix): rejected CMD18 with successful CMD12 falls back
  to single reads; failed CMD12 does not fall back, blocks later reads until `pf_sd_native_host_recover`. Dropping
  `!stop_ok` from the return condition makes these tests fail.

Commands (repository root, system Python 3 with pytest, Ceedling 1.1.10 with coverage tools on PATH):
`python3 -m pytest tests/contracts tests/pio tests/test_validation_integrity.py -q`;
`ceedling test:test_sd_native_host_read_batch`. Results are recorded in the change summary, not here.
