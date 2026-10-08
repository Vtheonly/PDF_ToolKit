"""pdftoolkit - a unified, headless PDF engine.

Merges the original PDF Toolkit, pdf-search and pdfm applications into one
modular backend with a single JSON envelope contract. No UI layer: this
package is consumed by frontends through :class:`Toolkit`, the CLI or the
optional HTTP adapter.
"""

from __future__ import annotations

from .core.errors import (
    ContentNotFoundError,
    DependencyUnavailableError,
    DocumentNotFoundError,
    OperationError,
    OutputConflictError,
    PageRangeError,
    PdfReadError,
    PdfToolkitError,
    ValidationError,
)
from .core.result import ENGINE_NAME, ENGINE_VERSION, engine_info
from .documents import DocumentRegistry
from .toolkit import Toolkit

__version__ = ENGINE_VERSION
__all__ = [
    "Toolkit",
    "DocumentRegistry",
    "ENGINE_NAME",
    "ENGINE_VERSION",
    "engine_info",
    "PdfToolkitError",
    "ValidationError",
    "PageRangeError",
    "OutputConflictError",
    "PdfReadError",
    "DocumentNotFoundError",
    "ContentNotFoundError",
    "DependencyUnavailableError",
    "OperationError",
    "__version__",
]
