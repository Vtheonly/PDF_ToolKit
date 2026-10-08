"""Typed error hierarchy with stable, machine-readable codes.

Every error raised by the engine derives from :class:`PdfToolkitError` and
carries:

* ``code``        - stable string token safe to switch on programmatically
* ``http_status`` - status code the HTTP adapter maps the error to
* ``message``     - human readable description
* ``details``     - structured, JSON-serialisable context (optional)
"""

from __future__ import annotations

from typing import Any, Dict, Optional


class PdfToolkitError(Exception):
    """Base class for every engine error."""

    code = "engine_error"
    http_status = 500

    def __init__(self, message: str, details: Optional[Dict[str, Any]] = None) -> None:
        super().__init__(message)
        self.message = message
        self.details = details or {}

    def to_dict(self) -> Dict[str, Any]:
        """Serialise the error into a JSON-safe dict."""
        return {"code": self.code, "message": self.message, "details": self.details}


class ValidationError(PdfToolkitError):
    """Invalid arguments supplied by the caller."""

    code = "invalid_input"
    http_status = 400


class PageRangeError(ValidationError):
    """Malformed or out-of-range page specification."""

    code = "invalid_page_range"


class OutputConflictError(ValidationError):
    """The requested output path collides with an input document."""

    code = "output_conflict"


class PdfReadError(PdfToolkitError):
    """A PDF could not be opened or parsed (corrupt, encrypted, empty)."""

    code = "unreadable_pdf"
    http_status = 400


class DocumentNotFoundError(PdfToolkitError):
    """A referenced document is not registered and does not exist on disk."""

    code = "document_not_found"
    http_status = 404


class ContentNotFoundError(PdfToolkitError):
    """A requested piece of content (e.g. keyword) does not exist."""

    code = "content_not_found"
    http_status = 404


class DependencyUnavailableError(PdfToolkitError):
    """An optional backend dependency is not installed."""

    code = "dependency_unavailable"
    http_status = 503


class OperationError(PdfToolkitError):
    """An operation failed for a reason not covered above."""

    code = "operation_failed"
    http_status = 500
