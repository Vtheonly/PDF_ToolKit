"""Input validation helpers shared by every layer.

These helpers are intentionally tiny: they normalise paths and assert caller
supplied preconditions, raising :class:`~pdftoolkit.core.errors.ValidationError`
subclasses with structured details on failure.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Union

from .errors import ValidationError

PathLike = Union[str, "os.PathLike"]

_KINDS = ("file", "dir", "any")


def resolve_path(
    value: PathLike,
    must_exist: bool = True,
    kind: str = "file",
) -> Path:
    """Normalise ``value`` into an absolute :class:`~pathlib.Path`.

    ``must_exist`` verifies presence on disk; ``kind`` restricts the value to
    ``"file"``, ``"dir"`` or ``"any"``.
    """
    if kind not in _KINDS:
        raise ValidationError(f"unknown path kind: {kind!r}")

    if value is None:
        raise ValidationError("a path is required", {"kind": kind})

    try:
        path = Path(os.fspath(value))
    except TypeError as exc:
        raise ValidationError(f"path is not a valid path-like value: {value!r}") from exc

    path = path.expanduser().resolve()

    if must_exist:
        if not path.exists():
            raise ValidationError(f"path does not exist: {path}", {"path": str(path)})
        if kind == "file" and not path.is_file():
            raise ValidationError(f"path is not a file: {path}", {"path": str(path)})
        if kind == "dir" and not path.is_dir():
            raise ValidationError(f"path is not a directory: {path}", {"path": str(path)})

    return path


def require(condition: bool, message: str, details: dict = None) -> None:
    """Raise :class:`ValidationError` unless ``condition`` holds."""
    if not condition:
        raise ValidationError(message, details or {})


def require_non_empty_str(value: Any, name: str) -> str:
    """Return ``value`` as a non-empty stripped string or fail."""
    if value is None:
        raise ValidationError(f"{name} is required")
    text = str(value).strip()
    if not text:
        raise ValidationError(f"{name} must not be empty")
    return text
