"""Filesystem helpers: discovery, collision-free output naming, text files."""

from __future__ import annotations

import os
from pathlib import Path
from typing import Iterable, List, Optional, Union

from ..core.errors import ValidationError
from ..core.validation import resolve_path
from .naming import natural_sorted

PathLike = Union[str, os.PathLike]

PDF_SUFFIX = ".pdf"


def find_pdfs(
    directory: PathLike,
    exclude: Optional[Iterable[PathLike]] = None,
    recursive: bool = False,
) -> List[Path]:
    """List PDF files in ``directory`` (natural sort, see :mod:`utils.naming`).

    ``exclude`` is a set of paths to skip, compared case-insensitively by
    absolute path. Non-PDF files are always ignored.
    """
    root = resolve_path(directory, must_exist=True, kind="dir")
    excluded = {str(resolve_path(p, must_exist=False, kind="any")).lower() for p in (exclude or [])}

    iterator = root.rglob("*") if recursive else root.iterdir()
    found = [
        path
        for path in iterator
        if path.is_file()
        and path.suffix.lower() == PDF_SUFFIX
        and str(path.resolve()).lower() not in excluded
    ]
    return natural_sorted(found, key=lambda p: p.name)


def ensure_parent(path: PathLike) -> Path:
    """Create the parent directory of ``path`` if missing and return it."""
    parent = Path(os.fspath(path)).expanduser().resolve().parent
    parent.mkdir(parents=True, exist_ok=True)
    return parent


def next_available_path(path: PathLike) -> Path:
    """Return ``path`` or ``name(1).ext``, ``name(2).ext`` ... if it exists."""
    candidate = Path(os.fspath(path)).expanduser()
    if not candidate.exists():
        return candidate
    stem = candidate.stem
    suffix = candidate.suffix
    parent = candidate.parent
    counter = 1
    while True:
        candidate = parent / f"{stem}({counter}){suffix}"
        if not candidate.exists():
            return candidate
        counter += 1


def read_text_file(path: PathLike) -> str:
    """Read a UTF-8 text file, raising :class:`ValidationError` if unreadable."""
    resolved = resolve_path(path, must_exist=True, kind="file")
    try:
        return resolved.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        raise ValidationError(f"cannot read text file: {resolved.name}", {"path": str(resolved)}) from exc
