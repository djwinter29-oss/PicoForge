"""load.sh Tcl quoting contract; uses a fake OpenOCD and never touches hardware."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.contracts

FAKE_OPENOCD = '#!/bin/sh\nfor a in "$@"; do printf "%s\\0" "$a"; done > "$FAKE_OPENOCD_ARGS"\n'
VALUES = ["plain", "with space", "br{ace}", "un{balanced", "back\\slash", "dollar$x [cmd] \"q\" ;semi", "{}\\{"]


def run_load(tmp_path, elf_name, serial, speed="5000"):
    fake = tmp_path / "fake-openocd"
    fake.write_text(FAKE_OPENOCD)
    fake.chmod(0o755)
    elf = tmp_path / elf_name
    elf.parent.mkdir(parents=True, exist_ok=True)
    elf.write_bytes(b"")
    out = tmp_path / "args"
    result = subprocess.run(
        ["sh", str(ROOT / "tools/firmware/load.sh"), "--skip-build", "--elf", str(elf), "--openocd-exe", str(fake),
         "--probe-serial", serial, "--adapter-speed-khz", speed],
        capture_output=True, text=True, env={"PATH": "/usr/bin:/bin", "FAKE_OPENOCD_ARGS": str(out)}, check=False)
    return elf, out, result


@pytest.mark.skipif(shutil.which("tclsh") is None, reason="tclsh required to evaluate the generated commands")
@pytest.mark.parametrize("value", VALUES)
def test_tcl_arguments_round_trip(tmp_path, value):
    elf, out, result = run_load(tmp_path, f"dir {value}/fw.elf".replace("/", "_", 1 if "/" in value else 0), value)
    assert result.returncode == 0, result.stderr
    args = out.read_bytes().split(b"\0")[:-1]
    commands = [args[i + 1].decode() for i, a in enumerate(args) if a == b"-c"]
    script = ("proc adapter {k v} {puts -nonewline \"$k\\0$v\\0\"}\n"
              "proc program {f args} {puts -nonewline \"program\\0$f\\0\"}\n"
              + "\n".join(commands))
    evaluated = subprocess.run(["tclsh"], input=script, capture_output=True, text=True, check=True).stdout
    fields = evaluated.split("\0")
    assert fields[:2] == ["adapter", "serial"] or fields[0] == "serial"
    assert value in fields
    assert str(elf) in fields


@pytest.mark.parametrize("separator", ["\n", "\r"])
@pytest.mark.parametrize("argument", ["serial", "elf"])
def test_newline_values_are_rejected_before_openocd(tmp_path, separator, argument):
    _, out, result = run_load(tmp_path, f"bad{separator}fw.elf" if argument == "elf" else "fw.elf",
                              f"bad{separator}serial" if argument == "serial" else "plain")
    assert result.returncode != 0
    assert "newline" in result.stderr
    assert not out.exists()


@pytest.mark.parametrize("speed", ["1", "5000", "0010", "999999999999999999999999999999"])
def test_valid_adapter_speed_is_one_argument(tmp_path, speed):
    _, out, result = run_load(tmp_path, "fw.elf", "plain", speed)
    assert result.returncode == 0, result.stderr
    args = out.read_bytes().split(b"\0")[:-1]
    assert args.count(f"adapter speed {speed}".encode()) == 1


@pytest.mark.parametrize("speed", ["", "0", "000", "-1", "+1", "abc", "1.5", "1 2", "1; shutdown", "1\nexit"])
def test_invalid_adapter_speed_fails_before_openocd(tmp_path, speed):
    _, out, result = run_load(tmp_path, "fw.elf", "plain", speed)
    assert result.returncode != 0
    assert "adapter speed must be a strictly positive decimal integer" in result.stderr
    assert not out.exists()


def test_openocd_without_target_config_fails_before_programming(tmp_path):
    _, out, _ = run_load(tmp_path, "fw.elf", "plain")
    (tmp_path / "fake-openocd").write_text(
        '#!/bin/sh\ncase "$*" in *find*) exit 1;; esac\nprintf x > "$FAKE_OPENOCD_ARGS"\n')
    out.unlink()
    result = subprocess.run(
        ["sh", str(ROOT / "tools/firmware/load.sh"), "--skip-build", "--board", "pico2_w", "--elf",
         str(tmp_path / "fw.elf"), "--openocd-exe", str(tmp_path / "fake-openocd")],
        capture_output=True, text=True, env={"PATH": "/usr/bin:/bin", "FAKE_OPENOCD_ARGS": str(out)}, check=False)
    assert result.returncode != 0
    assert "cannot find target/rp2350.cfg" in result.stderr
    assert "--openocd-exe" in result.stderr
    assert not out.exists()
