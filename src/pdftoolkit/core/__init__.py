"""Core layer: errors, results, validation, domain rules and models.

The core is pure Python: it never touches the PDF backend or the filesystem
beyond generic :mod:`pathlib` helpers.
"""

from .errors import (
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
from .result import ENGINE_NAME, ENGINE_VERSION, engine_info, failure, from_exception, success

__all__ = [
    "ContentNotFoundError",
    "DependencyUnavailableError",
    "DocumentNotFoundError",
    "OperationError",
    "OutputConflictError",
    "PageRangeError",
    "PdfReadError",
    "PdfToolkitError",
    "ValidationError",
    "ENGINE_NAME",
    "ENGINE_VERSION",
    "engine_info",
    "failure",
    "from_exception",
    "success",
]
