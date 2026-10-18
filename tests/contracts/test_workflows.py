"""Build/package workflow contracts; no runner, network or hardware access."""
from pathlib import Path
import re
import subprocess

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
