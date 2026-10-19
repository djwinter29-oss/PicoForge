"""Build/package workflow contracts; no runner, network or hardware access."""
from pathlib import Path
import re
import subprocess

import pytest

pytestmark = pytest.mark.contracts

ROOT = Path(__file__).resolve().parents[2]


def test_build_matrices_and_compatible_artifact_checks():
    for name in ("pr-check.yml", "release.yml"):
        workflow = (ROOT / ".github/workflows" / name).read_text()
        for dimension in ("board: [pico, pico2]", "clock: [150, 250]",
                          "transport: [SDIO_PIO, SPI]", "port: [FREERTOS, BAREMETAL]"):
            assert dimension in workflow
        if name == "release.yml":
            assert 'PF_SDIO_PIO_HIGH_SPEED: "ON"' in workflow
        assert '--sys-clk-mhz "${{ matrix.clock }}"' in workflow
        assert '--sd-card-transport "${{ matrix.transport }}"' in workflow
        assert '--sd-card-port "${{ matrix.port }}"' in workflow
        artifact_step = workflow.split("- name: Run project tests", 1)[1]
        assert "if: matrix.port == 'FREERTOS'" in artifact_step.split("run:", 1)[0]
        subprocess.run(["ruby", "-ryaml", "-e", "YAML.load_file(ARGV[0])",
                        str(ROOT / ".github/workflows" / name)], check=True)


def test_usb_protocol_documents_filesystem_input_report():
    doc = (ROOT / "docs/usb-protocol.md").read_text()
    section = " ".join(doc.split("## Input Report 7", 1)[1].split("\n## ", 1)[0].split())
    for text in ("3-byte payload", "excluding the report ID", "layout version is `2`", "1000 ms", "push-only",
                 "`GET_REPORT` is unsupported", "last successfully mounted", "not live mounted state",
                 "no padding", "modulo 256"):
        assert text in section
    for value, name in enumerate(("unknown", "FAT12", "FAT16", "FAT32", "exFAT")):
        assert f"| {value} | {name} |" in section


@pytest.mark.parametrize("name", ("pr-check.yml", "release.yml"))
def test_workflows_validate_baremetal_artifacts_after_build(name):
    workflow = (ROOT / ".github/workflows" / name).read_text()
    steps = workflow.split("      - name: ")[1:]
    artifact_steps = [step for step in steps if "tests/artifacts/test_sd_ports.py" in step]
    assert len(artifact_steps) == 1
    artifact_step = artifact_steps[0]
    assert artifact_step.split("run:", 1)[0].splitlines()[1].strip() == "if: matrix.port == 'BAREMETAL'"
    assert "python3 -m pytest tests/artifacts/test_sd_ports.py --baremetal-build-dir build/firmware" in artifact_step
    build_step = next(step for step in steps if step.startswith("Build firmware\n"))
    assert steps.index(build_step) < steps.index(artifact_step)
    if name == "release.yml":
        package_step = next(step for step in steps if step.startswith("Package firmware outputs\n"))
        assert steps.index(artifact_step) < steps.index(package_step)


def test_release_only_packages_workflow_artifacts():
    workflow = (ROOT / ".github/workflows/release.yml").read_text()
    assert "workflow_dispatch: {}" in workflow
    assert "contents: read" in workflow and "contents: write" not in workflow
    assert "actions/upload-artifact@" in workflow
    assert "sha256sum *.elf *.uf2 > SHA256SUMS" in workflow
    assert "if-no-files-found: error" in workflow
    assert "gh release" not in workflow and "upload-release" not in workflow


def test_stack_observation_uses_current_task_and_is_paced():
    config = (ROOT / 'firmware/src/integration/freertos/FreeRTOSConfig.h').read_text()
    assert '#define INCLUDE_uxTaskGetStackHighWaterMark 1' in config
    assert '#define configCHECK_FOR_STACK_OVERFLOW 2' in config
    for source in ('app/usb/usb_task.c', 'app/storage/storage_worker.c'):
        text = (ROOT / 'firmware/src' / source).read_text()
        assert 'pdMS_TO_TICKS(30000u)' in text
        assert 'uxTaskGetStackHighWaterMark(NULL)' in text
        assert 'minimum free stack: %u words' in text


@pytest.mark.parametrize("tag,valid", [
    ("v0.0.0", True), ("v1.2.3", True), ("v255.255.255", True),
    ("v256.0.0", False), ("v999999999999999999.0.0", False),
    ("v01.2.3", False), ("v1.02.3", False), ("v1.2.03", False),
    ("1.2.3", False), ("v1.2.3-dev", False), ("v1.2.3-rc1", False),
    ("v1.2.3\n", False), ("main", False), ("", False),
])
def test_release_tag_validation(tag, valid):
    result = subprocess.run([str(ROOT / "tools/firmware/release-version.sh"), tag],
                            capture_output=True, text=True)
    assert (result.returncode == 0) == valid
    if valid:
        assert result.stdout == tag[1:] + "\n"


def test_release_version_is_validated_before_every_build():
    workflow = (ROOT / ".github/workflows/release.yml").read_text()
    assert 'tags: ["v*.*.*"]' in workflow
    assert 'test "$RELEASE_REF_TYPE" = tag' in workflow
    assert 'tools/firmware/release-version.sh "$RELEASE_TAG"' in workflow
    assert "needs: validate-tag" in workflow
    assert "PF_FIRMWARE_VERSION: ${{ needs.validate-tag.outputs.version }}" in workflow
    assert "name: firmware-v${{ needs.validate-tag.outputs.version }}-${{ matrix.board }}-cpu${{ matrix.clock }}" in workflow


@pytest.mark.parametrize("name", ("pr-check.yml", "release.yml"))
def test_default_clock_builds_application_once_per_board_without_packaging_it(name):
    workflow = (ROOT / ".github/workflows" / name).read_text()
    step = workflow.split("- name: Build SDK-default clock application\n", 1)[1].split("\n      - name:", 1)[0]
    assert "if: matrix.clock == 150 && matrix.transport == 'SDIO_PIO' && matrix.port == 'FREERTOS'" in step
    assert 'tools/firmware/build.sh --board "${{ matrix.board }}"' in step
    assert "--sys-clk-mhz 0 --build-dir build/default-clock" in step
    assert "--sd-card-port BAREMETAL" not in step
    assert "cp build/firmware/*.elf build/firmware/*.uf2 package/" in workflow or name == "pr-check.yml"


def test_release_gates_packages_on_reusable_complete_host_validation():
    workflow = (ROOT / ".github/workflows/release.yml").read_text()
    assert "host-validation:\n    needs: validate-tag\n    uses: ./.github/workflows/host-validation.yml" in workflow
    assert "firmware:\n    needs: [validate-tag, host-validation]" in workflow
    host = (ROOT / ".github/workflows/host-validation.yml").read_text()
    assert "tools/validation/setup-ceedling-testing.sh test:all" in host
    assert "tools/validation/test-host.sh" in host
    runner = (ROOT / "tools/validation/test-host.sh").read_text()
    for suite in ("host/python/tests", "host/webhid/tests/*.test.mjs", "host/dotnet/PicoForge.sln"):
        assert suite in runner


def test_release_uses_commit_epoch_before_configuring_artifact_names():
    workflow = (ROOT / ".github/workflows/release.yml").read_text()
    epoch = 'echo "SOURCE_DATE_EPOCH=$(git show -s --format=%ct HEAD)" >> "$GITHUB_ENV"'
    assert epoch in workflow
    assert workflow.index(epoch) < workflow.index("- name: Build firmware")
