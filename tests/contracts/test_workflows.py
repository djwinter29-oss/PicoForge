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
        entries = re.findall(r"\{board: (\w+), transport: (\w+), port: (\w+)\}", workflow)
        assert len(entries) == 6 and len(set(entries)) == 6
        assert {e[0] for e in entries} == {"pico", "pico2"}
        assert {(e[1], e[2]) for e in entries} == {
            (transport, port) for transport in ("SPI", "SDIO_PIO") for port in ("FREERTOS", "BAREMETAL")
        }
        assert '--sd-card-transport "${{ matrix.transport }}"' in workflow
        assert '--sd-card-port "${{ matrix.port }}"' in workflow
        artifact_step = workflow.split("- name: Run project tests", 1)[1]
        assert "if: matrix.port == 'FREERTOS'" in artifact_step.split("run:", 1)[0]
        subprocess.run(["ruby", "-ryaml", "-e", "YAML.load_file(ARGV[0])",
                        str(ROOT / ".github/workflows" / name)], check=True)


def test_pr_check_validates_baremetal_artifacts_after_build():
    workflow = (ROOT / ".github/workflows/pr-check.yml").read_text()
    steps = workflow.split("      - name: ")[1:]
    artifact_steps = [step for step in steps if "tests/artifacts/test_sd_ports.py" in step]
    assert len(artifact_steps) == 1
    artifact_step = artifact_steps[0]
    assert artifact_step.split("run:", 1)[0].splitlines()[1].strip() == "if: matrix.port == 'BAREMETAL'"
    assert "python3 -m pytest tests/artifacts/test_sd_ports.py --baremetal-build-dir build/firmware" in artifact_step
    build_step = next(step for step in steps if step.startswith("Build firmware\n"))
    assert steps.index(build_step) < steps.index(artifact_step)


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
