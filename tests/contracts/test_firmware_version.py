"""PicoUart-compatible firmware version policy, independent of the SDK and toolchain."""
import os
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.contracts


@pytest.mark.parametrize(("version", "expected", "warning"), [
    ("0.0.0-dev", (0, 0, 0, 0), False),
    ("0.1.0", (0, 1, 0, 0x0001), False),
    ("1.2.3", (1, 2, 3, 0x0102), False),
    ("12.34.56-rc1", (12, 34, 56, 0x1234), False),
    ("1.2.3-rc123", (1, 2, 3, 0x0102), False),
    ("99.99.255", (99, 99, 255, 0x9999), False),
    ("100.0.0", (100, 0, 0, 0), True),
    ("0.100.0", (0, 100, 0, 0), True),
    ("255.255.255-dev", (255, 255, 255, 0), True),
])
def test_version_components_and_usb_bcd(version, expected, warning):
    result = subprocess.run(
        ["cmake", f"-DPF_FIRMWARE_VERSION={version}", "-P", str(ROOT / "firmware/version.cmake")],
        capture_output=True, text=True, check=False,
    )
    assert result.returncode == 0, result.stderr
    major, minor, patch, bcd = expected
    assert f"RESULT major={major} minor={minor} patch={patch} bcd={bcd}" in result.stderr
    assert ("CMake Warning" in result.stderr) == warning


@pytest.mark.parametrize("version", [
    "", "v1.2.3", "1.2", "1.2.3.4", "01.2.3", "1.02.3", "1.2.03",
    "-1.2.3", "1.2.3-beta", "1.2.3-rc0", "1.2.3-rc01", "1.2.3+build",
    "256.0.0", "0.256.0", "0.0.256", "999999999999999999999999.0.0",
])
def test_invalid_firmware_version_is_rejected(version):
    result = subprocess.run(
        ["cmake", f"-DPF_FIRMWARE_VERSION={version}", "-P", str(ROOT / "firmware/version.cmake")],
        capture_output=True, text=True, check=False,
    )
    assert result.returncode != 0
    assert "PF_FIRMWARE_VERSION" in result.stderr


@pytest.mark.parametrize(("environment_version", "arguments", "expected"), [
    (None, [], "0.0.0-dev"),
    ("2.3.4-dev", [], "2.3.4-dev"),
    ("2.3.4-dev", ["--firmware-version", "1.2.3-rc1"], "1.2.3-rc1"),
])
@pytest.mark.parametrize("script", ["build.sh", "load.sh"])
def test_firmware_scripts_pass_version_to_cmake(tmp_path, environment_version, arguments, expected, script):
    sdk = tmp_path / "sdk"
    (sdk / "external").mkdir(parents=True)
    (sdk / "external/pico_sdk_import.cmake").touch()
    kernel = tmp_path / "kernel"
    importer = kernel / "portable/ThirdParty/GCC/RP2040/FreeRTOS_Kernel_import.cmake"
    importer.parent.mkdir(parents=True)
    importer.touch()
    cmake = tmp_path / "cmake"
    cmake.write_text('#!/bin/sh\nprintf "%s\\n" "$@" >> "$CMAKE_ARGUMENT_LOG"\n')
    cmake.chmod(0o755)
    log = tmp_path / "arguments"
    build = tmp_path / "build"
    build.mkdir()
    elf = build / "pico_forge_rp2350_150_20261018.elf"
    elf.touch()
    (build / "pico_forge_elf.txt").write_text(f"{elf}\n")
    if script == "load.sh":
        probe = tmp_path / "openocd"
        probe.write_text('#!/bin/sh\nexit 0\n')
        probe.chmod(0o755)
        arguments = [*arguments, "--openocd-exe", str(probe)]
    env = dict(os.environ, PATH=f"{tmp_path}:{os.environ['PATH']}", CMAKE_ARGUMENT_LOG=str(log))
    env.pop("PF_FIRMWARE_VERSION", None)
    if environment_version is not None:
        env["PF_FIRMWARE_VERSION"] = environment_version
    subprocess.run(
        ["sh", str(ROOT / "tools/firmware" / script),
         "--board", "pico2", "--build-dir", str(build),
         "--generator", "Unix Makefiles", "--pico-sdk-path", str(sdk),
         "--freertos-kernel-path", str(kernel), *arguments],
        env=env, capture_output=True, text=True, check=True,
    )
    assert f"-DPF_FIRMWARE_VERSION={expected}" in log.read_text().splitlines()


@pytest.mark.parametrize("policy", ["ON", "OFF"])
def test_build_script_passes_explicit_sdio_policy(tmp_path, policy):
    sdk = tmp_path / "sdk/external"
    sdk.mkdir(parents=True)
    (sdk / "pico_sdk_import.cmake").touch()
    kernel = tmp_path / "kernel/portable/ThirdParty/GCC/RP2040"
    kernel.mkdir(parents=True)
    (kernel / "FreeRTOS_Kernel_import.cmake").touch()
    cmake = tmp_path / "cmake"
    cmake.write_text('#!/bin/sh\nprintf "%s\n" "$@" >> "$CMAKE_ARGUMENT_LOG"\n')
    cmake.chmod(0o755)
    log = tmp_path / "arguments"
    env = dict(os.environ, PATH=f"{tmp_path}:{os.environ['PATH']}",
               CMAKE_ARGUMENT_LOG=str(log), PF_SDIO_PIO_HIGH_SPEED=policy,
               FREERTOS_KERNEL_PATH=str(kernel.parents[3]))
    subprocess.run(["sh", str(ROOT / "tools/firmware/build.sh"), "--sdcard-transport", "SPI",
                    "--pico-sdk-path", str(sdk.parent)],
                   env=env, check=True, capture_output=True, text=True)
    arguments = log.read_text().splitlines()
    assert f"-DPF_SDIO_PIO_HIGH_SPEED={policy}" in arguments
    assert "-DPF_SYS_CLK_MHZ=0" in arguments
    assert "-DPF_SDCARD_PORT=FREERTOS" in arguments
    assert "-DPF_SDCARD_TRANSPORT=SPI" in arguments
    assert str(ROOT / "build/firmware-pico-SPI") in arguments


@pytest.mark.parametrize(("option", "value"), [
    ("--sys-clk-mhz", "200"),
    ("--sdcard-port", "BAREMETAL"),
])
def test_build_script_rejects_legacy_build_dimensions(option, value):
    result = subprocess.run(["sh", str(ROOT / "tools/firmware/build.sh"), option, value],
                            capture_output=True, text=True)
    assert result.returncode != 0
    assert f"Unknown argument: {option}" in result.stderr
