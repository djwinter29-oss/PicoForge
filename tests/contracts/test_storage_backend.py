"""Build-selected storage policy and safe explicit NOR region configuration."""
import os
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.contracts


@pytest.mark.parametrize("offset,size,valid", [
    ("0", "49152", True),
    ("0x1000", "0x10000", True),
    ("0", "16777216", True),
    ("16728064", "49152", True),
    ("", "49152", False),
    ("0", "", False),
    ("1", "65536", False),
    ("0", "49153", False),
    ("0", "45056", False),
    ("16732160", "49152", False),
    ("-4096", "65536", False),
    ("0", "not-a-size", False),
    ("0xfffffffffffff000", "65536", False),
    ("0", "0xfffffffffffff000", False),
])
def test_nor_region_thresholds(offset, size, valid):
    result = subprocess.run([
        "cmake", "-DPF_STORAGE_BACKEND=NOR", "-DPF_SD_CARD_PORT=FREERTOS",
        f"-DPF_NOR_REGION_OFFSET={offset}", f"-DPF_NOR_REGION_SIZE={size}",
        "-P", str(ROOT / "firmware/storage_backend.cmake"),
    ], text=True, capture_output=True, check=False)
    assert (result.returncode == 0) == valid, result.stderr
    if not valid:
        assert "storage_backend.cmake" in result.stderr


@pytest.mark.parametrize("backend,port,valid", [
    ("SD", "FREERTOS", True),
    ("SD", "BAREMETAL", True),
    ("NOR", "BAREMETAL", False),
    ("UNKNOWN", "FREERTOS", False),
])
def test_backend_selection(backend, port, valid):
    result = subprocess.run([
        "cmake", f"-DPF_STORAGE_BACKEND={backend}", f"-DPF_SD_CARD_PORT={port}",
        "-DPF_NOR_REGION_OFFSET=0", "-DPF_NOR_REGION_SIZE=65536",
        "-P", str(ROOT / "firmware/storage_backend.cmake"),
    ], text=True, capture_output=True, check=False)
    assert (result.returncode == 0) == valid, result.stderr


@pytest.mark.parametrize("arguments,expected_backend,expected_smoke", [
    ([], "SD", "OFF"),
    (["--nor-transport", "QSPI_PIO"], "SD", "ON"),
    (["--storage-backend", "NOR", "--nor-transport", "QSPI_PIO",
      "--nor-region-offset", "4096", "--nor-region-size", "65536"], "NOR", "OFF"),
    (["--nor-transport", "QSPI_PIO", "--storage-backend", "NOR",
      "--nor-region-offset", "4096", "--nor-region-size", "65536"], "NOR", "OFF"),
])
def test_build_wrapper_preserves_smoke_and_selects_application(
        tmp_path, arguments, expected_backend, expected_smoke):
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
    env = dict(os.environ, PATH=f"{tmp_path}:{os.environ['PATH']}", CMAKE_ARGUMENT_LOG=str(log))
    for key in ("PF_STORAGE_BACKEND", "PF_NOR_REGION_OFFSET", "PF_NOR_REGION_SIZE", "PF_SD_CARD_PORT"):
        env.pop(key, None)
    subprocess.run([
        "sh", str(ROOT / "tools/firmware/build.sh"), "--board", "pico2",
        "--build-dir", str(tmp_path / "build"), "--generator", "Unix Makefiles",
        "--pico-sdk-path", str(sdk), "--freertos-kernel-path", str(kernel), *arguments,
    ], env=env, text=True, capture_output=True, check=True)
    values = log.read_text().splitlines()
    assert f"-DPF_STORAGE_BACKEND={expected_backend}" in values
    assert f"-DPF_BUILD_NOR_SMOKE={expected_smoke}" in values
    if expected_backend == "NOR":
        assert "-DPF_NOR_REGION_OFFSET=4096" in values
        assert "-DPF_NOR_REGION_SIZE=65536" in values


def test_load_wrapper_preserves_explicit_nor_selection_without_accessing_hardware(tmp_path):
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
    probe = tmp_path / "openocd"
    probe.write_text('#!/bin/sh\nprintf "called\\n" >> "$PROBE_CALL_LOG"\n')
    probe.chmod(0o755)
    build = tmp_path / "build"
    build.mkdir()
    (build / "pf_hello_world.elf").touch()
    log = tmp_path / "arguments"
    probe_log = tmp_path / "probe-calls"
    env = dict(os.environ, PATH=f"{tmp_path}:{os.environ['PATH']}", CMAKE_ARGUMENT_LOG=str(log),
               PROBE_CALL_LOG=str(probe_log))
    command = [
        "sh", str(ROOT / "tools/firmware/load.sh"), "--board", "pico2", "--build-dir", str(build),
        "--pico-sdk-path", str(sdk), "--freertos-kernel-path", str(kernel),
        "--openocd-exe", str(probe), "--generator", "Unix Makefiles",
        "--storage-backend", "NOR", "--nor-transport", "QSPI_PIO",
        "--nor-region-offset", "4096", "--nor-region-size", "65536",
    ]
    subprocess.run(command, env=env, text=True, capture_output=True, check=True)
    values = log.read_text().splitlines()
    assert "-DPF_STORAGE_BACKEND=NOR" in values
    assert "-DPF_BUILD_NOR_SMOKE=OFF" in values
    assert "-DPF_NOR_TRANSPORT=QSPI_PIO" in values
    assert "-DPF_NOR_REGION_OFFSET=4096" in values
    assert "-DPF_NOR_REGION_SIZE=65536" in values
    assert probe_log.read_text() == "called\n"
    (build / "CMakeCache.txt").write_text("PF_STORAGE_BACKEND:STRING=SD\n")
    result = subprocess.run(command + ["--skip-build"], env=env, text=True, capture_output=True, check=False)
    assert result.returncode != 0
    assert "different storage backend" in result.stderr
    assert probe_log.read_text() == "called\n"
