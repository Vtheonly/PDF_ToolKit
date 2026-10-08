"""Domain models shared across the engine.

All models are plain dataclasses with ``to_dict`` so they serialise to
JSON without adapters knowing about their internals.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Tuple


@dataclass(frozen=True)
class DocumentMeta:
    """A document known to the engine (registered or resolved from disk)."""

    id: str
    path: str
    filename: str
    page_count: Optional[int] = None

    def to_dict(self) -> Dict[str, Any]:
        return {
            "id": self.id,
            "path": self.path,
            "filename": self.filename,
            "page_count": self.page_count,
        }


@dataclass(frozen=True)
class SearchHit:
    """A single keyword match summary for one page."""

    page: int
    keyword: str
    count: int
    snippet: Optional[str] = None

    def to_dict(self) -> Dict[str, Any]:
        return {
            "page": self.page,
            "keyword": self.keyword,
            "count": self.count,
            "snippet": self.snippet,
        }


@dataclass(frozen=True)
class LinkInfo:
    """An outbound hyperlink found on a page."""

    page: int
    uri: str
    rect: Tuple[float, float, float, float] = (0.0, 0.0, 0.0, 0.0)

    def to_dict(self) -> Dict[str, Any]:
        return {"page": self.page, "uri": self.uri, "rect": list(self.rect)}


@dataclass(frozen=True)
class MergeSource:
    """One contributing file of a merge operation."""

    path: str
    filename: str
    pages: int = 0

    def to_dict(self) -> Dict[str, Any]:
        return {"path": self.path, "filename": self.filename, "pages": self.pages}
