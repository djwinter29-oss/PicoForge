#!/usr/bin/env python3
"""Verify an installed wheel with its interpreter, outside the source import path and without hardware."""

import argparse
from importlib.metadata import version
from importlib.resources import files
from importlib.util import find_spec
from pathlib import Path
import subprocess
import sys
import sysconfig


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", help="expected installed distribution and displayed application version")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    assert sys.flags.isolated, "Run this check with python -I to exclude checkout imports and PYTHONPATH"

    import picoforge
    from picoforge.dashboard.boards import DashboardBoards
    from picoforge.dashboard.server import create_app
    from picoforge.hid.device import PicoForgeHid
    from picoforge.reports import parse_identity_report

    assert picoforge.__file__ is not None, "The installed distribution must be a regular package"
    assert not Path(picoforge.__file__).resolve().is_relative_to(root / "host/python")
    assert version("picoforge") == args.version
    assert picoforge.__version__ == args.version
    assert PicoForgeHid.__module__ == "picoforge.hid.device"
    assert parse_identity_report.__module__ == "picoforge.reports.identity"
    for name in ("console", "device", "monitor", "protocol"):
        assert find_spec(f"picoforge.{name}") is None, name
    source = root / "host/web/src"
    expected = {path.relative_to(source).as_posix(): path.read_bytes()
                for path in source.rglob("*") if path.is_file()}
    web = files("picoforge").joinpath("web")

    def assets(directory, prefix=""):
        result = {}
        for path in directory.iterdir():
            name = prefix + path.name
            if path.is_dir():
                result.update(assets(path, name + "/"))
            else:
                result[name] = path.read_bytes()
        return result

    assert assets(web) == expected, "Installed web payload must contain exactly the source assets"
    boards = DashboardBoards(discover=lambda: [])
    app = create_app(dashboard=boards)
    try:
        client = app.test_client()
        for route, title in (("/", "PicoForge Dashboard | Python"), ("/console", "PicoForge Console | Python")):
            response = client.get(route)
            assert response.status_code == 200
            assert title.encode() in response.data
            assert f"App v{args.version}<".encode() in response.data
        for asset, content in expected.items():
            if asset.endswith(".html"):
                continue
            with client.get("/" + asset) as response:
                assert response.status_code == 200, asset
                assert response.data == content, asset
        assert client.get("/src/js/app.mjs").status_code == 404
        assert client.get("/api/boards").json == {"boards": [], "default_board_id": None}
    finally:
        app.extensions["picoforge_console"].close_all()
        boards.close()

    for module in ("picoforge.reports", "picoforge.reports.models", "picoforge.hid",
                   "picoforge.hid.models", "picoforge.dashboard.console", "picoforge.dashboard.server"):
        subprocess.run([sys.executable, "-I", "-c", f"import {module}"], check=True, timeout=10)
    subprocess.run([sys.executable, "-I", "-m", "picoforge", "--help"], check=True, timeout=10)
    scripts = Path(sysconfig.get_path("scripts"))
    executable = scripts / ("picoforge.exe" if sys.platform == "win32" else "picoforge")
    subprocess.run([str(executable), "--help"], check=True, timeout=10)
    print(f"Installed PicoForge {args.version}: imports, entry points, templates and {len(expected)} assets verified")


if __name__ == "__main__":
    main()
