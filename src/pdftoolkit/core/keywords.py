"""Keyword matching and aggregation primitives.

Matching is customisable along two axes, unifying the behaviours of the
original applications:

* ``case_sensitive``  - ``False`` (default) matches across letter case, as
  both the original pdf-search backend and pdftoolkit did;
* ``whole_words``     - ``True`` (default) matches on word boundaries so that
  searching for ``alpha`` does not match ``alphabet`` (the hardened
  pdftoolkit behaviour); ``False`` reproduces the substring semantics of the
  original pdf-search backend.

Both axes are honoured everywhere: single-document search, corpus search,
keyword anchors and the one-shot extract-matches workflow.
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


def keyword_pattern(
    keyword: str,
    case_sensitive: bool = False,
    whole_words: bool = True,
) -> "re.Pattern[str]":
    """Compile a pattern for ``keyword`` honouring both match axes."""
    escaped = re.escape(keyword)
    body = rf"\b{escaped}\b" if whole_words else escaped
    flags = 0 if case_sensitive else re.IGNORECASE
    return re.compile(body, flags)


def count_keywords(
    text: str,
    keywords: Iterable[str],
    case_sensitive: bool = False,
    whole_words: bool = True,
) -> Dict[str, int]:
    """Count non-overlapping occurrences of each keyword in ``text``."""
    counts: Dict[str, int] = {}
    for keyword in keywords:
        counts[keyword] = len(
            keyword_pattern(keyword, case_sensitive, whole_words).findall(text or "")
        )
    return counts


def contains_keyword(
    text: str,
    keyword: str,
    case_sensitive: bool = False,
    whole_words: bool = True,
) -> bool:
    """Return whether ``text`` contains ``keyword``."""
    return keyword_pattern(keyword, case_sensitive, whole_words).search(text or "") is not None


def first_match_snippet(
    text: str,
    keyword: str,
    width: int = _SNIPPET_WIDTH,
    case_sensitive: bool = False,
    whole_words: bool = True,
) -> Optional[str]:
    """Return a short excerpt of ``text`` around the first match, or ``None``."""
    match = keyword_pattern(keyword, case_sensitive, whole_words).search(text or "")
    if match is None:
        return None
    start = max(0, match.start() - width)
    end = min(len(text), match.end() + width)
    prefix = "..." if start > 0 else ""
    suffix = "..." if end < len(text) else ""
    return f"{prefix}{text[start:end].strip()}{suffix}"


def keywords_with_matches(counts: Dict[str, int]) -> List[str]:
    """Return the keywords whose count in ``counts`` is positive."""
    return [keyword for keyword, count in counts.items() if count > 0]


def aggregate_totals(counts: Iterable[Dict[str, int]]) -> Dict[str, int]:
    """Sum a sequence of per-item keyword count dicts into totals."""
    totals: Dict[str, int] = {}
    for entry in counts:
        for keyword, count in entry.items():
            totals[keyword] = totals.get(keyword, 0) + count
    return totals
