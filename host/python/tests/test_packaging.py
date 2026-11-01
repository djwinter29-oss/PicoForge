import unittest
from fnmatch import fnmatch
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:  # Python 3.10
    tomllib = None

PROJECT = Path(__file__).resolve().parents[1]


class PackagingTests(unittest.TestCase):
    @unittest.skipIf(tomllib is None, "tomllib requires Python 3.11")
    def test_package_metadata_includes_repository_license(self):
        project = tomllib.loads((PROJECT / "pyproject.toml").read_text())["project"]
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
        self.assertEqual(web, PROJECT.parent / "web")
        self.assertIn("picoforge.web", setuptools["packages"])
        patterns = setuptools["package-data"]["picoforge.web"]
        assets = [p.relative_to(web).as_posix() for p in web.rglob("*")
                  if p.is_file() and not {"__pycache__", "tests"}.intersection(p.parts)]
        self.assertTrue(assets)
        for asset in assets:
            self.assertTrue(any(fnmatch(asset, pattern) for pattern in patterns), asset)
        self.assertIn("js/wireless-form.mjs", assets)
        self.assertEqual(setuptools["packages"], ["picoforge", "picoforge.web"])
