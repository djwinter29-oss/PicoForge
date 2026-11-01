"""Build/package workflow contracts; no runner, network or hardware access."""
from pathlib import Path
import json
import os
import re
import subprocess

import pytest

pytestmark = pytest.mark.contracts

ROOT = Path(__file__).resolve().parents[2]


def test_build_matrices_and_compatible_artifact_checks():
    for name in ("pr-check.yml", "release.yml"):
        workflow = (ROOT / ".github/workflows" / name).read_text()
        for dimension in ("board: [pico, pico_w, pico2, pico2_w]", "transport: [SDIO_PIO, SPI]"):
            assert dimension in workflow
        for removed in ("BAREMETAL", "matrix.clock", "matrix.port",
                        "--sys-clk-mhz", "--sdcard-port", "Build SDK-default clock application"):
            assert removed not in workflow
        assert re.search(r"^\s+clock:\s*\[", workflow, re.MULTILINE) is None
        assert re.search(r"^\s+port:\s*\[", workflow, re.MULTILINE) is None
        assert "PF_SDIO_PIO_HIGH_SPEED" not in workflow
        assert '--sdcard-transport "${{ matrix.transport }}"' in workflow
        artifact_step = workflow.split("- name: Run project tests", 1)[1]
        assert "if:" not in artifact_step.split("run:", 1)[0]
        subprocess.run(["ruby", "-ryaml", "-e", "YAML.load_file(ARGV[0])",
                        str(ROOT / ".github/workflows" / name)], check=True)


def test_hid_report_reference_documents_filesystem_storage_field():
    doc = (ROOT / "docs/design/usb/usb-hid-report-reference.md").read_text()
    section = " ".join(doc.split("## Report ID 10: Storage Status", 1)[1].split("\n## ", 1)[0].split())
    for text in ("**Total** | **34**", "`filesystem_format`",
                 "Last successfully mounted format", "not proof that a volume is currently mounted",
                 "nominally every 500 ms", "modulo 256"):
        assert text in section
    assert "Filesystem format values: `0` Unknown, `1` FAT12, `2` FAT16, `3` FAT32, `4` exFAT" in section


@pytest.mark.parametrize("name", ("pr-check.yml", "release.yml"))
def test_workflows_run_pio_timing_only_for_sdio(name):
    workflow = (ROOT / ".github/workflows" / name).read_text()
    steps = workflow.split("      - name: ")[1:]
    common = next(step for step in steps if step.startswith("Run project tests\n"))
    assert "if:" not in common.split("run:", 1)[0]
    assert "tests/pio" not in common
    for suite in ("tests/contracts", "tests/test_validation_integrity.py", "tests/artifacts/test_msc_elf.py"):
        assert suite in common
    assert '--elf "$(cat "build/firmware/pico_forge_elf.txt")"' in common
    assert "--junitxml=build/project-tests.xml" in common

    timing_steps = [step for step in steps if "tests/pio" in step]
    assert len(timing_steps) == 1
    timing = timing_steps[0]
    assert timing.startswith("Run SDIO PIO timing tests\n")
    assert timing.split("run:", 1)[0].splitlines()[1].strip() == "if: matrix.transport == 'SDIO_PIO'"
    assert "python3 -m pytest tests/pio --junitxml=build/pio-timing-tests.xml" in timing
    assert "continue-on-error" not in timing
    build = next(step for step in steps if step.startswith("Build firmware\n"))
    assert steps.index(build) < steps.index(common) < steps.index(timing)


def test_release_publishes_only_after_all_packages_succeed():
    workflow = (ROOT / ".github/workflows/release.yml").read_text()
    assert "preflight does not create a tag or release" in workflow
    assert workflow.split("jobs:", 1)[0].count("contents: read") == 1
    assert workflow.count("contents: write") == 1
    assert "actions/upload-artifact@" in workflow
    assert "sha256sum *.elf *.uf2 > SHA256SUMS" in workflow
    assert "if-no-files-found: error" in workflow
    assert "needs: [validate-tag, firmware, python, webhid, dotnet]" in workflow
    assert not (ROOT / ".github/workflows/package-host.yml").exists()
    assert "softprops/action-gh-release@efb35369e0ad2afab669f228072c1b0d510eae64" in workflow
    assert "sha256sum * > SHA256SUMS.txt" in workflow
    assert "uses: ./.github/workflows/publish-dotnet.yml" in workflow
    assert "gh-action-pypi-publish@" not in workflow
    assert "publish-webhid.yml" not in workflow


@pytest.mark.parametrize("event,ref_type,ref_name,input_tag,expected", (
    ("push", "tag", "v1.2.3", "", "1.2.3"),
    ("push", "branch", "main", "", None),
    ("push", "tag", "v01.2.3", "", None),
    ("workflow_dispatch", "branch", "main", "v2.3.4", "2.3.4"),
    ("workflow_dispatch", "tag", "v1.2.3", "v2.3.4", "2.3.4"),
    ("workflow_dispatch", "branch", "main", "", None),
    ("workflow_dispatch", "branch", "main", "v256.0.0", None),
))
def test_release_preflight_resolves_version_without_publishing(
    event, ref_type, ref_name, input_tag, expected, tmp_path,
):
    result = subprocess.run(
        ["ruby", "-ryaml", "-rjson", "-e", "puts JSON.generate(YAML.load_file(ARGV[0]))",
         str(ROOT / ".github/workflows/release.yml")],
        check=True, capture_output=True, text=True,
    )
    workflow = json.loads(result.stdout)
    jobs = workflow["jobs"]
    assert jobs["release"]["if"] == "github.event_name == 'push'"
    assert workflow["on"]["workflow_dispatch"]["inputs"]["tag"]["required"] is True
    steps = jobs["validate-tag"]["steps"]
    script = next(step["run"] for step in steps if step.get("id") == "version")
    output = tmp_path / "output"
    result = subprocess.run(
        ["bash", "-e", "-c", script], cwd=ROOT, capture_output=True, text=True,
        env={**os.environ, "EVENT_NAME": event, "RELEASE_REF_TYPE": ref_type, "RELEASE_TAG": ref_name,
             "INPUT_TAG": input_tag, "GITHUB_OUTPUT": str(output)},
    )
    if expected is not None:
        assert result.returncode == 0, result.stderr
        assert output.read_text() == f"version={expected}\n"
    else:
        assert result.returncode != 0
        assert not output.exists()
    publish = next(step for step in jobs["release"]["steps"] if step.get("name") == "Publish GitHub release")
    assert publish["with"] == {
        "tag_name": "${{ github.ref_name }}",
        "name": "PicoForge ${{ github.ref_name }}",
        "generate_release_notes": True,
        "draft": False,
        "prerelease": False,
        "files": "dist/*",
        "fail_on_unmatched_files": True,
    }


@pytest.mark.parametrize("name", ("publish-dotnet.yml", "publish-pypi.yml", "publish-webhid.yml"))
def test_publication_workflows_have_expected_triggers_and_pinned_actions(name):
    path = ROOT / ".github/workflows" / name
    workflow = path.read_text()
    subprocess.run(["ruby", "-ryaml", "-e", "YAML.load_file(ARGV[0])", str(path)], check=True)
    if name == "publish-dotnet.yml":
        assert "workflow_call:" in workflow
    else:
        assert "workflow_call:" not in workflow
        assert "workflows: [Release build packages]" in workflow
        assert "github.event.workflow_run.conclusion == 'success'" in workflow
        assert "github.event.workflow_run.event == 'push'" in workflow
        assert "github.event.workflow_run.head_repository.full_name == github.repository" in workflow
        assert 'test "$(git rev-parse \'FETCH_HEAD^{commit}\')" = "$SOURCE_SHA"' in workflow
        assert "sha256sum --check SHA256SUMS.txt --ignore-missing" in workflow
    assert "workflow_dispatch:" in workflow
    for action in re.findall(r"uses:\s+([^\s]+)", workflow):
        assert re.search(r"@[0-9a-f]{40}$", action), action


def test_host_packages_and_publishers_share_validated_artifacts():
    packages = (ROOT / ".github/workflows/release.yml").read_text()
    dotnet = (ROOT / ".github/workflows/publish-dotnet.yml").read_text()
    for rid in ("linux-x64", "win-x64", "osx-arm64"):
        assert f"rid: {rid}" in dotnet
    assert '--self-contained true' in dotnet
    assert "shutil.unpack_archive(archive, directory)" in dotnet
    assert 'assert b"PicoForge Dashboard | .NET" in response.read()' in dotnet
    assert 'assert version("picoforge") == os.environ["VERSION"]' in packages
    assert 'web = files("picoforge").joinpath("web")' in packages
    assert 'assert b"PicoForge Dashboard | Python" in response.data' in packages
    for name, artifact, pattern in (
        ("pypi", "release-python", "picoforge-$version-py3-none-any.whl"),
        ("webhid", "release-webhid", "picoforge-webhid-v$version.zip"),
    ):
        publish = (ROOT / f".github/workflows/publish-{name}.yml").read_text()
        assert f"name: {artifact}" in packages
        assert f'--pattern "{pattern}"' in publish
        assert "gh release download" in publish
    assert "gh-action-pypi-publish@" in (ROOT / ".github/workflows/publish-pypi.yml").read_text()
    assert "deploy-pages@" in (ROOT / ".github/workflows/publish-webhid.yml").read_text()


@pytest.mark.parametrize("name", (
    "release.yml", "publish-dotnet.yml", "publish-pypi.yml", "publish-webhid.yml",
))
def test_publication_embedded_scripts_compile(name):
    path = ROOT / ".github/workflows" / name
    result = subprocess.run(
        ["ruby", "-ryaml", "-rjson", "-e", "puts JSON.generate(YAML.load_file(ARGV[0]))", str(path)],
        check=True, capture_output=True, text=True,
    )
    workflow = json.loads(result.stdout)
    for job in workflow["jobs"].values():
        for step in job.get("steps", []):
            if "run" not in step:
                continue
            subprocess.run(["bash", "-n"], input=step["run"], text=True, check=True)
            for source in re.findall(r"python - <<'PY'\n(.*?)\nPY", step["run"], re.DOTALL):
                compile(source, f"{name}:{step.get('name', 'run')}", "exec")


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
    assert "name: firmware-v${{ needs.validate-tag.outputs.version }}-${{ matrix.board }}-${{ matrix.transport }}" in workflow


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


def test_host_validation_covers_three_os_and_two_python_versions():
    workflow = (ROOT / ".github/workflows/host-validation.yml").read_text()
    for operating_system in ("ubuntu-24.04", "macos-15", "windows-2025"):
        for python_version in ("3.10", "3.12"):
            assert f"os: {operating_system}\n            python: \"{python_version}\"" in workflow
    assert "if: runner.os == 'Linux'" in workflow
    assert "if: runner.os == 'macOS'" in workflow
    assert "run: brew install hidapi" in workflow
    assert "PYTHON: python" in workflow
    assert "shell: bash\n        run: bash tools/validation/test-host.sh" in workflow
    assert "if: matrix.python == '3.10'" in workflow
    assert "if: matrix.python == '3.12'" in workflow


def test_release_uses_commit_epoch_before_configuring_artifact_names():
    workflow = (ROOT / ".github/workflows/release.yml").read_text()
    epoch = 'echo "SOURCE_DATE_EPOCH=$(git show -s --format=%ct HEAD)" >> "$GITHUB_ENV"'
    assert epoch in workflow
    assert workflow.index(epoch) < workflow.index("- name: Build firmware")
