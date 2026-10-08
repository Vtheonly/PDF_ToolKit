"""Naming helpers, most notably natural (human) ordering.

Natural ordering ensures ``1_alpha.pdf`` < ``2_beta.pdf`` < ``10_zeta.pdf``
instead of the lexicographic ``1`` < ``10`` < ``2``.
"""

from __future__ import annotations

import os
import re
from pathlib import Path
from typing import Any, Callable, List, Optional, Sequence, Tuple, TypeVar

T = TypeVar("T")

_NUMBER_RE = re.compile(r"(\d+)")


def natural_key(value: str) -> List[Any]:
    """Split a string into lowercase text / integer chunks for sorting.

    Empty chunks (produced by ``re.split`` at the string boundaries) are
    dropped so a pure number yields a single integer chunk. The result is
    the documented introspection contract; for *sorting* use
    :func:`sortable_key`, which wraps the mixed chunks into a type-safe
    tuple so ``int`` and ``str`` chunks never compare against each other.
    """
    parts = _NUMBER_RE.split(str(value).lower())
    return [int(part) if part.isdigit() else part for part in parts if part]


def sortable_key(value: str) -> Tuple[Tuple[int, Any], ...]:
    """Type-safe sort key built from :func:`natural_key`'s chunks.

    Each chunk becomes ``(0, chunk)`` for integers and ``(1, chunk)`` for
    strings, so comparisons are always well defined no matter how text and
    number chunks are interleaved.
    """
    return tuple(
        (0, chunk) if isinstance(chunk, int) else (1, chunk)
        for chunk in natural_key(value)
    )


def natural_sorted(
    values: Sequence[T],
    key: Optional[Callable[[T], str]] = None,
) -> List[T]:
    """Sort values naturally, preserving stability for equal keys."""
    items = list(values)
    if key is None:
        return sorted(items, key=lambda item: sortable_key(str(item)))
    return sorted(items, key=lambda item: sortable_key(key(item)))


def stem(path: "os.PathLike") -> str:
    """Return the file name of ``path`` without its suffix."""
    return Path(os.fspath(path)).stem


def render_filename(prefix: Optional[str], page: int) -> str:
    """Build a deterministic output name for a rendered page image."""
    base = prefix if prefix else "page"
    return f"{base}-page-{int(page):04d}.png"
