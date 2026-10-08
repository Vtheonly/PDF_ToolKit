"""Shared base for all engine services.

Services own business logic, never envelope construction: they raise typed
errors and return plain dicts. The :class:`~pdftoolkit.toolkit.Toolkit`
facade (and only it) turns results into JSON envelopes.
"""

from __future__ import annotations

from ..documents.registry import DocumentRegistry


class Service:
    """Base service: shared registry access for every capability."""

    def __init__(self, registry: DocumentRegistry) -> None:
        self._registry = registry

    @property
    def registry(self) -> DocumentRegistry:
        return self._registry
