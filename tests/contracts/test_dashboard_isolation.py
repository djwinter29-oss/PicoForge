"""Keep Direct and the Python/.NET frontend independently deployable."""
from pathlib import Path
import re

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.contracts


@pytest.mark.parametrize("dashboard", ("direct", "web"))
def test_dashboard_modules_and_assets_stay_inside_their_dashboard(dashboard):
    root = ROOT / "host" / dashboard
    for source in root.rglob("*"):
        if source.suffix not in (".mjs", ".html") or "__pycache__" in source.parts:
            continue
        text = source.read_text()
        if source.suffix == ".mjs":
            references = re.findall(r'^\s*import\s+(?:[^;]*?\bfrom\s+)?["\']([^"\']+)', text, re.MULTILINE)
            references += re.findall(r'\bimport\s*\(\s*["\']([^"\']+)', text)
        else:
            references = re.findall(r'\b(?:src|href)=["\']([^"\']+)', text)
        for reference in references:
            if not reference.startswith(".") and ":" in reference:
                continue
            if reference.startswith("#"):
                continue
            base = root if reference.startswith("/") else source.parent
            target = (base / reference.split("?", 1)[0].split("#", 1)[0].lstrip("/")).resolve()
            assert target.is_relative_to(root.resolve()), f"{source}: external dependency {reference}"
            assert target.exists(), f"{source}: missing dependency {reference}"


def test_native_hosts_do_not_package_or_serve_direct_assets():
    project = (ROOT / "host/python/pyproject.toml").read_text()
    server = (ROOT / "host/python/src/picoforge/dashboard/server.py").read_text()
    dotnet = (ROOT / "host/dotnet/PicoForge/PicoForge.csproj").read_text()
    for source in (project, server, dotnet):
        for dependency in ("../direct", "/direct/", '"direct"', "'direct'", "wireless_ui"):
            assert dependency not in source
    assert (ROOT / "host/web/js/wireless-form.mjs").is_file()
    assert (ROOT / "host/direct/js/wireless-form.mjs").is_file()
