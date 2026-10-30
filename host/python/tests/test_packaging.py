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
    def test_wheel_packages_browser_bluetooth_static_assets(self):
        setuptools = tomllib.loads((PROJECT / "pyproject.toml").read_text())["tool"]["setuptools"]
        web = (PROJECT / setuptools["package-dir"]["picoforge.ble"]).resolve()
        self.assertEqual(web, PROJECT.parent / "webhid")
        self.assertIn("picoforge.ble", setuptools["packages"])
        patterns = setuptools["package-data"]["picoforge.ble"]
        assets = [web / "index.html", web / "favicon.svg",
                  *web.glob("css/*.css"), *web.glob("js/*.mjs")]
        for asset in assets:
            relative = asset.relative_to(web).as_posix()
            self.assertTrue(any(fnmatch(relative, pattern) for pattern in patterns), relative)

    @unittest.skipIf(tomllib is None, "tomllib requires Python 3.11")
    def test_wheel_packages_every_dashboard_asset_from_host_web(self):
        setuptools = tomllib.loads((PROJECT / "pyproject.toml").read_text())["tool"]["setuptools"]
        web = (PROJECT / setuptools["package-dir"]["picoforge.web"]).resolve()
        self.assertEqual(web, PROJECT.parent / "web")
        self.assertIn("picoforge.web", setuptools["packages"])
        patterns = setuptools["package-data"]["picoforge.web"]
        assets = [p.relative_to(web).as_posix() for p in web.rglob("*") if p.is_file() and "__pycache__" not in p.parts]
        self.assertTrue(assets)
        for asset in assets:
            self.assertTrue(any(fnmatch(asset, pattern) for pattern in patterns), asset)
