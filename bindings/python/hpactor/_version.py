"""HPActor version — read from package metadata or fall back."""

from importlib.metadata import PackageNotFoundError, version as _metadata_version

try:
    __version__ = _metadata_version("hpactor")
except PackageNotFoundError:
    __version__ = "0+unknown"

__all__ = ["__version__"]
