import unittest
import subprocess
import sys
from fnmatch import fnmatch
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:  # Python 3.10
    tomllib = None

PROJECT = Path(__file__).resolve().parents[1]


class PackagingTests(unittest.TestCase):
    def test_public_packages_import_independently_without_hardware(self):
        for module in ("picoforge.reports", "picoforge.dashboard.console",
                       "picoforge.hid", "picoforge.dashboard.server", "picoforge.reports.models",
                       "picoforge.hid.models"):
            with self.subTest(module=module):
                result = subprocess.run(
                    [sys.executable, "-c", f"import {module}"], capture_output=True, text=True, timeout=10,
                )
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_legacy_model_imports_keep_the_same_report_types(self):
        from picoforge.hid import models as legacy
        from picoforge.reports import models

        for name in legacy.__all__:
            with self.subTest(name=name):
                self.assertIs(getattr(legacy, name), getattr(models, name))

    def test_public_apis_live_in_their_domain_packages(self):
        from picoforge.dashboard import console as dashboard_console
        from picoforge.dashboard import monitor as dashboard_monitor
        from picoforge.dashboard import server
        from picoforge.hid import device as hid_device
        import picoforge.reports as reports

        self.assertNotIn("struct", reports.__all__)
        self.assertEqual(hid_device.PicoForgeHid.__module__, "picoforge.hid.device")
        self.assertEqual(dashboard_monitor.DeviceMonitor.__module__, "picoforge.dashboard.monitor")
        self.assertEqual(dashboard_console.ConsoleBridge.__module__, "picoforge.dashboard.console")
        self.assertEqual(server.create_app.__module__, "picoforge.dashboard.server")
        self.assertIn("parse_identity_report", reports.__all__)
        self.assertIn("parse_wireless_status_report", reports.__all__)
        self.assertIn("parse_memory_report", reports.__all__)
        for alias in ("parse_telemetry_report", "parse_storage_report",
                      "parse_self_test_status_report", "parse_tcp_cli_auth_result"):
            self.assertNotIn(alias, reports.__all__)

    def test_compatibility_modules_are_removed(self):
        package = PROJECT / "src" / "picoforge"
        for module in ("console", "device", "monitor", "protocol", "web"):
            self.assertFalse((package / f"{module}.py").exists(), module)
        self.assertFalse((package / "reports" / "hid.py").exists())

    @unittest.skipIf(tomllib is None, "tomllib requires Python 3.11")
    def test_lock_matches_declared_runtime_dependencies(self):
        project = tomllib.loads((PROJECT / "pyproject.toml").read_text())["project"]
        packages = tomllib.loads((PROJECT / "uv.lock").read_text())["package"]
        locked = next(package for package in packages if package["name"] == project["name"])
        declared = {requirement.lower() for requirement in project["dependencies"]}
        self.assertEqual(
            {requirement["name"] + requirement["specifier"] for requirement in locked["metadata"]["requires-dist"]},
            declared,
        )
        for dependency in declared:
            name, version = dependency.split("==")
            self.assertEqual(next(package["version"] for package in packages if package["name"] == name), version)

    @unittest.skipIf(tomllib is None, "tomllib requires Python 3.11")
    def test_package_metadata_includes_repository_license(self):
        project = tomllib.loads((PROJECT / "pyproject.toml").read_text())["project"]
        self.assertEqual(project["version"], "0.0.0.dev0")
        packages = tomllib.loads((PROJECT / "uv.lock").read_text())["package"]
        self.assertEqual(next(package["version"] for package in packages if package["name"] == "picoforge"),
                         project["version"])
        self.assertEqual(project["license"], "BSD-3-Clause")
        self.assertEqual(project["license-files"], ["LICENSE"])
        self.assertEqual((PROJECT / "LICENSE").read_bytes(), (PROJECT.parents[1] / "LICENSE").read_bytes())
        self.assertEqual(project["scripts"], {"picoforge": "picoforge.cli:main"})
        self.assertEqual(project["requires-python"], ">=3.10")
        self.assertIn("Documentation", project["urls"])
        self.assertIn("Source", project["urls"])

    @unittest.skipIf(tomllib is None, "tomllib requires Python 3.11")
    def test_wheel_packages_every_dashboard_asset_from_host_web(self):
        setuptools = tomllib.loads((PROJECT / "pyproject.toml").read_text())["tool"]["setuptools"]
        web = (PROJECT / setuptools["package-dir"]["picoforge.web"]).resolve()
        self.assertEqual(web, PROJECT.parent / "web/src")
        self.assertIn("picoforge.web", setuptools["packages"])
        patterns = setuptools["package-data"]["picoforge.web"]
        assets = [p.relative_to(web).as_posix() for p in web.rglob("*")
                  if p.is_file() and not {"__pycache__", "tests"}.intersection(p.parts)]
        self.assertTrue(assets)
        for asset in assets:
            self.assertTrue(any(fnmatch(asset, pattern) for pattern in patterns), asset)
        self.assertIn("js/wireless-form.mjs", assets)
        self.assertEqual(
            setuptools["packages"],
            ["picoforge", "picoforge.web", "picoforge.reports", "picoforge.hid", "picoforge.dashboard"],
        )
