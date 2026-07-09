"""HPActor package version.

Reads the installed distribution version via importlib.metadata.
Falls back to "0+unknown" when the package is not installed (e.g. build tree).
"""

from importlib.metadata import PackageNotFoundError, version

try:
    __version__ = version("hpactor")
except PackageNotFoundError:
    __version__ = "0+unknown"

__all__ = ["__version__"]
