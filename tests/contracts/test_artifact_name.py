"""Firmware output names and flash selection without hardware."""
import os
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.contracts


@pytest.mark.parametrize(("platform", "clock", "label"), [
    ("RP2040", 0, "rp2040_125"),
    ("RP2350", 0, "rp2350_150"),
    ("RP2040", 200, "rp2040_200"),
    ("RP2350", 250, "rp2350_250"),
])
@pytest.mark.parametrize("epoch,date,timezone", [
    ("1792324800", "20261018", "Pacific/Kiritimati"),
    ("1792411200", "20261019", "America/Los_Angeles"),
])
def test_artifact_name_uses_platform_clock_and_utc_date(tmp_path, platform, clock, label, epoch, date, timezone):
    script = tmp_path / "name.cmake"
    script.write_text(
        f'set(PICO_{platform} 1)\nset(PF_SYS_CLK_MHZ {clock})\n'
        f'include("{ROOT / "firmware/artifact_name.cmake"}")\n'
        'message("NAME=${PF_ARTIFACT_NAME}")\n'
    )
    result = subprocess.run(
        ["cmake", "-P", str(script)],
        env=dict(os.environ, SOURCE_DATE_EPOCH=epoch, TZ=timezone),
        capture_output=True, text=True, check=True,
    )
    assert f"NAME=pico_forge_{label}_{date}" in result.stderr


def test_loader_uses_manifest_not_stale_images_and_fails_without_it(tmp_path):
    build = tmp_path / "build with spaces"
    build.mkdir()
    elf = build / "pico_forge_rp2350_250_20261018.elf"
    elf.touch()
    (build / "pico_forge_rp2350_150_20261017.elf").touch()
    probe_log = tmp_path / "probe.log"
    probe = tmp_path / "openocd"
    probe.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$PROBE_LOG"\n')
    probe.chmod(0o755)
    command = [
        "sh", str(ROOT / "tools/firmware/load.sh"), "--board", "pico2", "--skip-build",
        "--build-dir", str(build), "--openocd-exe", str(probe),
    ]
    env = dict(os.environ, PROBE_LOG=str(probe_log))
    result = subprocess.run(command, env=env, text=True, capture_output=True)
    assert result.returncode != 0
    assert "Firmware ELF path missing" in result.stderr
    assert not probe_log.exists()
    (build / "pico_forge_elf.txt").write_text(f"{elf}\n")
    subprocess.run(command, env=env, text=True, capture_output=True, check=True)
    escaped_elf = str(elf).replace(" ", "\\ ")
    assert f"program {escaped_elf} verify reset exit" in probe_log.read_text()
    elf.unlink()
    result = subprocess.run(command, env=env, text=True, capture_output=True)
    assert result.returncode != 0
    assert "ELF file not found" in result.stderr
