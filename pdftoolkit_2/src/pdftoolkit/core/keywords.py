"""Keyword matching and aggregation primitives.

Matching is case-insensitive and word-boundary aware so that searching for
``alpha`` does not match ``alphabet``.
"""

from __future__ import annotations

import re
from typing import Dict, Iterable, List, Optional

from .errors import ValidationError
from .validation import require_non_empty_str

_SNIPPET_WIDTH = 40


def normalize_keywords(keywords: Iterable[str]) -> List[str]:
    """Validate and de-duplicate keywords, preserving first-seen order."""
    if keywords is None:
        raise ValidationError("keywords are required")

    if isinstance(keywords, str):
        keywords = [keywords]

    try:
        raw = list(keywords)
    except TypeError as exc:
        raise ValidationError(f"keywords must be an iterable of strings: {keywords!r}") from exc

    if not raw:
        raise ValidationError("at least one keyword is required")

    normalized: List[str] = []
    for keyword in raw:
        text = require_non_empty_str(keyword, "keyword")
        if text not in normalized:
            normalized.append(text)
    return normalized


def keyword_pattern(keyword: str) -> "re.Pattern[str]":
    """Compile a word-boundary, case-insensitive pattern for ``keyword``."""
    return re.compile(r"\b" + re.escape(keyword) + r"\b", re.IGNORECASE)


def count_keywords(text: str, keywords: Iterable[str]) -> Dict[str, int]:
    """Count non-overlapping occurrences of each keyword in ``text``."""
    counts: Dict[str, int] = {}
    for keyword in keywords:
        counts[keyword] = len(keyword_pattern(keyword).findall(text or ""))
    return counts


def contains_keyword(text: str, keyword: str) -> bool:
    """Return whether ``text`` contains ``keyword`` (case-insensitive)."""
    return keyword_pattern(keyword).search(text or "") is not None


def first_match_snippet(text: str, keyword: str, width: int = _SNIPPET_WIDTH) -> Optional[str]:
    """Return a short excerpt of ``text`` around the first match, or ``None``."""
    match = keyword_pattern(keyword).search(text or "")
    if match is None:
        return None
    start = max(0, match.start() - width)
    end = min(len(text), match.end() + width)
    prefix = "..." if start > 0 else ""
    suffix = "..." if end < len(text) else ""
    return f"{prefix}{text[start:end].strip()}{suffix}"


def aggregate_totals(counts: Iterable[Dict[str, int]]) -> Dict[str, int]:
    """Sum a sequence of per-item keyword count dicts into totals."""
    totals: Dict[str, int] = {}
    for entry in counts:
        for keyword, count in entry.items():
            totals[keyword] = totals.get(keyword, 0) + count
    return totals
