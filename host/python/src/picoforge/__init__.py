"""PicoForge host tools."""
import importlib.metadata
from pathlib import Path


def _resolve_package_version(distribution_name: str, package_file: str) -> str:
    """Return installed distribution version, or 0.0.0 for source-tree imports."""
    try:
        distribution = importlib.metadata.distribution(distribution_name)
    except importlib.metadata.PackageNotFoundError:
        return "0.0.0-debug"
    package_path = Path(package_file).resolve()
    distribution_root = Path(str(distribution.locate_file(""))).resolve()
    try:
        package_path.relative_to(distribution_root)
    except ValueError:
        return "0.0.0-debug"
    return distribution.version


__version__ = _resolve_package_version("picoforge", __file__)

__all__ = ["__version__"]