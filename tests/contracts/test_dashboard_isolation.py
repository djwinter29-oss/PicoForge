"""Keep Direct and the Python/.NET frontend independently deployable."""
from pathlib import Path
import hashlib
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
            asset_root = root / "src" if dashboard == "web" else root
            base = asset_root if reference.startswith("/") else source.parent
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
    assert (ROOT / "host/web/src/js/wireless-form.mjs").is_file()
    assert (ROOT / "host/direct/web/js/ui/sections/wireless-form.mjs").is_file()


def test_shared_dashboard_sources_and_source_matched_tests_have_separate_roots():
    web = ROOT / "host/web"
    for old in ("index.html", "console.html", "favicon.svg", "js", "css"):
        assert not (web / old).exists(), f"Old shared asset location remains: {old}"
    assert (web / "tsconfig.json").is_file()
    assert (web / "tests/run-tests.mjs").is_file()
    for source in (web / "src/js").glob("*.mjs"):
        suite = web / "tests" / ("integration" if source.stem == "app" else "js") / f"{source.stem}.test.mjs"
        assert suite.is_file(), f"No source-matched suite for {source.name}"
    for source in ("index", "console", "favicon"):
        assert (web / f"tests/{source}.test.mjs").is_file()
    project = (ROOT / "host/dotnet/PicoForge/PicoForge.csproj").read_text()
    assert 'Content Include="../../web/src/**/*"' in project
    assert 'Content Include="../../web/**/*"' not in project


def test_direct_bridge_imports_nothing_from_native_hosts():
    source = (ROOT / "host/direct/server.py").read_text()
    modules = re.findall(r"^\s*(?:from|import)\s+([\w.]+)", source, re.MULTILINE)
    assert not [module for module in modules if module.split(".")[0] in ("picoforge", "host", "tools")]


def _file_digests(*roots):
    skipped = {"__pycache__", "build", "bin", "obj", "node_modules"}
    files = (path for root in roots for path in ([root] if root.is_file() else root.rglob("*")))
    return {hashlib.sha256(path.read_bytes()).hexdigest(): path for path in files
            if path.is_file() and path.stat().st_size and not skipped.intersection(path.parts)}


def test_direct_shares_no_file_copies_with_other_hosts_except_branding_favicon():
    direct = _file_digests(ROOT / "host/direct")
    others = _file_digests(*(ROOT / "host" / name for name in ("web", "python", "dotnet")))
    copies = sorted(f"{direct[digest].relative_to(ROOT)} == {others[digest].relative_to(ROOT)}"
                    for digest in direct.keys() & others.keys()
                    if not (direct[digest] == ROOT / "host/direct/web/favicon.svg"
                            and others[digest] == ROOT / "host/web/src/favicon.svg"))
    assert not copies, "Direct must own its code and assets instead of copying them"
