"""The single, uniform JSON result envelope used across the whole engine.

Every public operation - programmatic, CLI or HTTP - returns exactly this
shape::

    {
        "ok": true | false,
        "operation": "<operation name>",
        "data": { ... } | null,
        "error": { "code", "message", "details" } | null,
        "engine": { "name", "version" }
    }
"""

from __future__ import annotations

from typing import Any, Dict, Optional

from .errors import OperationError, PdfToolkitError

ENGINE_NAME = "pdftoolkit"
ENGINE_VERSION = "1.0.0"


def engine_info() -> Dict[str, str]:
    """Identity block attached to every envelope."""
    return {"name": ENGINE_NAME, "version": ENGINE_VERSION}


def success(operation: str, data: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    """Build a success envelope."""
    return {
        "ok": True,
        "operation": operation,
        "data": data if data is not None else {},
        "error": None,
        "engine": engine_info(),
    }


def failure(operation: str, error: PdfToolkitError) -> Dict[str, Any]:
    """Build a failure envelope from a typed engine error."""
    return {
        "ok": False,
        "operation": operation,
        "data": None,
        "error": error.to_dict(),
        "engine": engine_info(),
    }


def from_exception(operation: str, exc: BaseException) -> Dict[str, Any]:
    """Build a failure envelope, wrapping unknown exceptions safely."""
    if isinstance(exc, PdfToolkitError):
        return failure(operation, exc)
    wrapped = OperationError(str(exc) or exc.__class__.__name__)
    return failure(operation, wrapped)
