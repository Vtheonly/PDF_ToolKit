"""Service layer: one module per capability, one shared base class.

The catalog below is the single source of truth for what the engine can do;
the toolkit and the HTTP adapter surface it as the ``capabilities`` API.
"""

from __future__ import annotations

from typing import Any, Dict, Optional

from ..documents.registry import DocumentRegistry
from .base import Service
from .links import LinksService
from .merge import MergeService
from .page_ops import PageOpsService
from .render import RenderService
from .search import SearchService
from .speech import SpeechProvider, SpeechService
from .text import TextService

SERVICE_CATALOG: Dict[str, Dict[str, Any]] = {
    "search": {
        "service": "SearchService",
        "description": "Keyword search with per-page hits, snippets and totals",
        "operations": ["search", "search_corpus"],
    },
    "text": {
        "service": "TextService",
        "description": "Extract page text from documents",
        "operations": ["extract_text"],
    },
    "links": {
        "service": "LinksService",
        "description": "Extract outbound hyperlinks from documents",
        "operations": ["extract_links"],
    },
    "page_ops": {
        "service": "PageOpsService",
        "description": "Cut page selections and expand keyword-anchored context",
        "operations": ["cut_pages", "context_pages", "extract_context"],
    },
    "merge": {
        "service": "MergeService",
        "description": "Concatenate PDFs, explicitly or from whole folders",
        "operations": ["merge", "merge_folder"],
    },
    "render": {
        "service": "RenderService",
        "description": "Rasterise pages to PNG images or files",
        "operations": ["render_pages", "render_to_files"],
    },
    "speech": {
        "service": "SpeechService",
        "description": "Text-to-speech synthesis with an injectable provider",
        "operations": ["synthesize_speech"],
    },
}


def build_services(
    registry: DocumentRegistry,
    speech_provider: Optional[SpeechProvider] = None,
) -> Dict[str, Service]:
    """Instantiate every service against one shared registry."""
    return {
        "search": SearchService(registry),
        "text": TextService(registry),
        "links": LinksService(registry),
        "page_ops": PageOpsService(registry),
        "merge": MergeService(registry),
        "render": RenderService(registry),
        "speech": SpeechService(registry, provider=speech_provider),
    }


def catalog_as_dict() -> Dict[str, Dict[str, Any]]:
    """Return a deep, JSON-safe copy of the service catalog."""
    import copy

    return copy.deepcopy(SERVICE_CATALOG)


def all_operations() -> list:
    """Return every operation name exposed by the catalog."""
    operations = []
    for entry in SERVICE_CATALOG.values():
        operations.extend(entry["operations"])
    return sorted(operations)


__all__ = [
    "Service",
    "SearchService",
    "TextService",
    "LinksService",
    "PageOpsService",
    "MergeService",
    "RenderService",
    "SpeechService",
    "SpeechProvider",
    "SERVICE_CATALOG",
    "build_services",
    "catalog_as_dict",
    "all_operations",
]
