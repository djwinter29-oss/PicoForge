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
workflow uploads Actions artifacts, then publishes a
[GitHub release](https://github.com/djwinter29-oss/PicoForge/releases) only after all firmware and host packages succeed.
Firmware packages are attached as ZIPs, preserving the board/transport distinction even when image basenames match.
The release also includes:

- `picoforge-X.Y.Z-py3-none-any.whl`: Python CLI and local dashboard.
- `picoforge-host-vX.Y.Z-linux-x64.tar.gz`, `picoforge-host-vX.Y.Z-win-x64.zip`, and
  `picoforge-host-vX.Y.Z-osx-arm64.tar.gz`: self-contained .NET hosts, including dashboard assets.
- `picoforge-webhid-vX.Y.Z.zip`: standalone browser dashboard.
- `SHA256SUMS.txt`: SHA-256 checksums for all attached packages.

The release workflow builds the Python wheel and WebHID archive directly and calls `publish-dotnet.yml` to build
standalone hosts. The release job uploads all files to a draft before making it public. A failed upload leaves a draft that can be
completed by rerunning that job; an already public release is not overwritten. Builds and uploads alone do not deploy
anything. After release publication, separate jobs publish the same tested wheel to PyPI using Trusted Publishing
and deploy the same WebHID archive to GitHub Pages. They do not rebuild packages from a moving branch.
`publish-pypi.yml` and `publish-webhid.yml` run independently when **Release build packages** completes successfully
in this repository. Both require a public, stable release, match its tag to the originating workflow commit,
and verify the downloaded asset against the release checksums. A failure in either workflow does not remove the release.
They also accept an existing published release tag through manual dispatch for retries; neither rebuilds the asset.
`publish-dotnet.yml` supports manual packaging of an existing tag as Actions artifacts without modifying a release.
Rerun only failed jobs after fixing external configuration; PyPI versions are immutable.

Packaging is gated on the reusable host-validation workflow: Ceedling firmware tests, Python host tests on
3.10 and 3.12, and WebHID and .NET host tests. Each PR/release matrix entry performs one firmware build; runtime
clock selection remains a boot setting and does not multiply CI artifacts.

## Publishing setup

Before pushing the first release tag:

1. Enable GitHub Actions and permit the release job to write repository contents. Other build jobs remain read-only.
2. In **Settings → Pages**, select **GitHub Actions** as the source. Allow the `github-pages` environment to deploy
   the default branch, where the post-release publishing workflows run. The intended URL is
   <https://djwinter29-oss.github.io/PicoForge/>.
3. On PyPI, configure a Trusted Publisher (or pending publisher for a new project) for project `picoforge`, owner
   `djwinter29-oss`, repository `PicoForge`, workflow `publish-pypi.yml`, and environment `pypi`.
   The package name must be available or owned by the maintainer; do not assume it is reserved.
4. Create the GitHub `pypi` environment, restrict it to the default branch, and configure any required reviewer approval.
   No PyPI API-token secret is needed. Only publication jobs receive OIDC token permissions.
5. Review and push a valid release tag. Manual **Release build packages** dispatch must select an existing valid tag.
   Monitor the release, PyPI, and Pages jobs separately before announcing downloads.

These workflows configure publication; they do not provision PyPI ownership, change repository settings, sign host
binaries, or establish hardware qualification. Download and hosted-site links may be unavailable before the first run.
There is no branch/development publication path: manual PyPI/Pages dispatch also requires a public release tag.
The PyPI job intentionally runs in the standalone `publish-pypi.yml` workflow because
[PyPI does not support reusable workflows as Trusted Publishers](https://docs.pypi.org/trusted-publishers/troubleshooting/#reusable-workflows-on-github).

## Using downloads

Download the matching board/transport firmware or host archive from
[the latest release](https://github.com/djwinter29-oss/PicoForge/releases/latest). Verify against `SHA256SUMS.txt`
before extracting; for example, use `sha256sum --check SHA256SUMS.txt --ignore-missing` on Linux.

Install Python with `python -m pip install picoforge` in a virtual environment, then run `picoforge web`.
The wheel attached to the release can also be installed directly with pip.

For the .NET host, extract the entire archive and run `PicoForge.exe` (Windows x64) or `./PicoForge`
(Linux x64/macOS ARM64), then open <http://127.0.0.1:5000/>. No .NET SDK/runtime installation is required.
Pass `--port 5001` if port 5000 is occupied. Linux still needs native HIDAPI (`libhidapi-hidraw0` on Debian/Ubuntu)
and [HID permissions](../../tools/README.md); macOS needs HIDAPI (`brew install hidapi`).
Unsigned binaries may trigger Windows SmartScreen or macOS Gatekeeper warnings; no code-signing/notarization is claimed.
Only run trusted, verified downloads and follow local security policy.

Use [hosted WebHID](https://djwinter29-oss.github.io/PicoForge/) in desktop Chrome/Edge, or extract the WebHID ZIP
and run `python -m http.server 8000 --directory PATH_TO_EXTRACTED_SITE`, then open <http://localhost:8000/>.
Localhost or HTTPS and device permission are required. Avoid competing dashboard connections to one board.

## Build and hardware limits

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
divider planning. SPI follows its separate card-rate and divider policy. See [SD design](../design/sdcard/sdcard-driver-design.md).

Product release remains blocked on allocation/owner approval of a USB identity. `0xCAFE:0x4011` is the documented
development VID/PID and remains unchanged; it must not be represented as an allocated product identity.
Board/card/wiring and clock combinations also require hardware qualification before product shipment. Software CI
and historical lab records do not qualify every release configuration. No hardware validation is claimed here.
