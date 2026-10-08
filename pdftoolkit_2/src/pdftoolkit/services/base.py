"""Shared base for all engine services.

Services own business logic, never envelope construction: they raise typed
errors and return plain dicts. The :class:`~pdftoolkit.toolkit.Toolkit`
facade (and only it) turns results into JSON envelopes.
"""

from __future__ import annotations

from typing import ContextManager

from ..documents.registry import DocumentRegistry
from ..io.pdfio import PdfDocument, open_pdf
from ..core.models import DocumentMeta


class Service:
    """Base service: registry access plus a document-opening helper."""

    def __init__(self, registry: DocumentRegistry) -> None:
        self._registry = registry

    @property
    def registry(self) -> DocumentRegistry:
        return self._registry

    def open_document(self, reference) -> ContextManager[PdfDocument]:
        """Resolve ``reference`` and open its PDF (usable with ``with``)."""
        meta: DocumentMeta = self._registry.resolve(reference)
        return open_pdf(meta.path)
