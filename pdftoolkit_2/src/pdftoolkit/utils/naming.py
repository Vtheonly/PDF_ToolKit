"""Naming helpers, most notably natural (human) ordering.

Natural ordering ensures ``1_alpha.pdf`` < ``2_beta.pdf`` < ``10_zeta.pdf``
instead of the lexicographic ``1`` < ``10`` < ``2``.
"""

from __future__ import annotations

import os
import re
from pathlib import Path
from typing import Any, Callable, Iterable, List, Optional, Sequence, TypeVar

T = TypeVar("T")

_NUMBER_RE = re.compile(r"(\d+)")


def natural_key(value: str) -> List[Any]:
    """Split a string into lowercase text / integer chunks for sorting."""
    parts = _NUMBER_RE.split(str(value).lower())
    return [int(part) if part.isdigit() else part for part in parts]


def natural_sorted(
    values: Sequence[T],
    key: Optional[Callable[[T], str]] = None,
) -> List[T]:
    """Sort values naturally, preserving stability for equal keys."""
    items = list(values)
    if key is None:
        return sorted(items, key=lambda item: natural_key(str(item)))
    return sorted(items, key=lambda item: natural_key(key(item)))


def stem(path: "os.PathLike") -> str:
    """Return the file name of ``path`` without its suffix."""
    return Path(os.fspath(path)).stem


def render_filename(prefix: Optional[str], page: int) -> str:
    """Build a deterministic output name for a rendered page image."""
    base = prefix if prefix else "page"
    return f"{base}-{int(page):04d}.png"
