"""Version resolution contracts independent of installed packages and hardware."""

import importlib.metadata
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest.mock import Mock, patch

PACKAGE = Path(__file__).resolve().parents[1] / "src" / "picoforge" / "__init__.py"


class VersionTests(unittest.TestCase):
    def test_missing_distribution_uses_source_fallback(self):
        with patch("importlib.metadata.distribution", side_effect=importlib.metadata.PackageNotFoundError) as lookup:
            package = runpy.run_path(str(PACKAGE))
        lookup.assert_called_once_with("picoforge")
        self.assertEqual(package["__version__"], "0.0.0-debug")

    def test_installed_version_is_used_only_for_the_owning_package_location(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            installed = root / "site-packages"
            package_path = installed / "picoforge" / "__init__.py"
            package_path.parent.mkdir(parents=True)
            package_path.write_bytes(PACKAGE.read_bytes())
            distribution = Mock(version="1.2.3")
            distribution.locate_file.return_value = installed
            with patch("importlib.metadata.distribution", return_value=distribution):
                package = runpy.run_path(str(package_path))
                self.assertEqual(package["__version__"], "1.2.3")
                resolve = package["_resolve_package_version"]
                self.assertEqual(resolve("picoforge", str(root / "checkout" / "picoforge" / "__init__.py")),
                                 "0.0.0-debug")
                self.assertEqual(resolve("picoforge", str(root / "site-packages-other" / "__init__.py")),
                                 "0.0.0-debug", "a matching path prefix is not distribution ownership")

    def test_unexpected_metadata_errors_are_not_hidden(self):
        with patch("importlib.metadata.distribution", side_effect=OSError("Metadata unreadable")):
            with self.assertRaisesRegex(OSError, "Metadata unreadable"):
                runpy.run_path(str(PACKAGE))
