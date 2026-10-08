"""Page specification parsing and windowing.

Pages are **1-based** everywhere in the public API; conversion to the
0-based indexing used by the PDF backend happens exclusively inside
:mod:`pdftoolkit.io.pdfio`.

Supported page specification syntax (strict):

* ``None`` / ``""`` / ``"0"`` / ``"all"``  -> every page
* ``"3"``                                 -> a single page
* ``"1-5"``                               -> an inclusive range
* ``"1,3,5-7"``                           -> combination of the above

Anything else - reversed ranges, out-of-range pages, empty tokens, unknown
tokens - is rejected with :class:`~pdftoolkit.core.errors.PageRangeError`.
"""

from __future__ import annotations

from typing import Iterable, List, Union

from .errors import PageRangeError

PageSpec = Union[None, int, str, Iterable[int]]

_ALL_TOKENS = {"0", "all", "*"}


def parse_page_spec(spec: PageSpec, total_pages: int) -> List[int]:
    """Parse ``spec`` into a sorted list of unique 1-based page numbers.

    Raises :class:`PageRangeError` for malformed or out-of-range input.
    """
    if not isinstance(total_pages, int) or total_pages < 1:
        raise PageRangeError("document has no readable pages", {"total_pages": total_pages})

    if spec is None:
        return list(range(1, total_pages + 1))

    if isinstance(spec, bool):
        raise PageRangeError(f"invalid page specification: {spec!r}")

    if isinstance(spec, int):
        pages: List[int] = [spec]
    elif isinstance(spec, str):
        pages = _parse_spec_string(spec, total_pages)
    else:
        try:
            pages = list(spec)
        except TypeError as exc:
            raise PageRangeError(f"invalid page specification: {spec!r}") from exc

    unique: List[int] = []
    for page in pages:
        if isinstance(page, bool) or not isinstance(page, int):
            raise PageRangeError(f"page must be an integer, got {page!r}")
        if page < 1 or page > total_pages:
            raise PageRangeError(
                f"page {page} is out of range 1..{total_pages}",
                {"page": page, "total_pages": total_pages},
            )
        if page not in unique:
            unique.append(page)

    return sorted(unique)


def _parse_spec_string(spec: str, total_pages: int) -> List[int]:
    text = spec.strip()
    if not text or text.lower() in _ALL_TOKENS:
        return list(range(1, total_pages + 1))

    if text.lower() == "none":
        return []

    pages: List[int] = []
    for token in text.split(","):
        token = token.strip()
        if not token:
            raise PageRangeError(f"empty page token in specification: {spec!r}", {"spec": spec})
        pages.extend(_parse_token(token, spec))
    return pages


def _parse_token(token: str, spec: str) -> List[int]:
    if "-" in token:
        left, sep, right = token.partition("-")
        start = _parse_int(left.strip(), spec, token)
        end = _parse_int(right.strip(), spec, token)
        if start > end:
            raise PageRangeError(
                f"reversed page range: {token!r}", {"spec": spec, "token": token}
            )
        return list(range(start, end + 1))
    return [_parse_int(token, spec, token)]


def _parse_int(text: str, spec: str, token: str) -> int:
    if not text.isdigit():
        raise PageRangeError(
            f"invalid page token: {token!r} in specification {spec!r}",
            {"spec": spec, "token": token},
        )
    return int(text)


def format_pages(pages: Iterable[int]) -> str:
    """Render a page list as a compact human-readable spec (``1-3,7``)."""
    ordered = sorted(set(pages))
    parts: List[str] = []
    run_start = 0
    for index, page in enumerate(ordered):
        is_last = index == len(ordered) - 1
        if is_last or ordered[index + 1] != page + 1:
            run_end = page
            if run_end == ordered[run_start]:
                parts.append(str(run_end))
            else:
                parts.append(f"{ordered[run_start]}-{run_end}")
            run_start = index + 1
    return ",".join(parts)


def context_window(
    page: int,
    total_pages: int,
    before: int = 1,
    after: int = 1,
) -> List[int]:
    """Expand ``page`` to include up to ``before``/``after`` neighbouring pages.

    The window is clamped to the document bounds and always contains the
    anchor page itself.
    """
    if total_pages < 1:
        raise PageRangeError("document has no readable pages", {"total_pages": total_pages})
    if not isinstance(page, int) or isinstance(page, bool) or page < 1 or page > total_pages:
        raise PageRangeError(
            f"page {page} is out of range 1..{total_pages}",
            {"page": page, "total_pages": total_pages},
        )
    for name, value in (("before", before), ("after", after)):
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            raise PageRangeError(f"{name} must be a non-negative integer, got {value!r}")

    start = max(1, page - before)
    end = min(total_pages, page + after)
    return list(range(start, end + 1))


def expand_with_padding(
    pages: Iterable[int],
    total_pages: int,
    padding: int = 2,
) -> List[int]:
    """Expand every page in ``pages`` by ``padding`` neighbours on both sides.

    This is the unified version of the original pdf-search backend's
    ``calculate_page_range``: each matched page contributes a clamped
    ``[page - padding, page + padding]`` window, overlapping windows merge,
    duplicates collapse and the result is a sorted list of unique 1-based
    page numbers ready for extraction.

    Raises :class:`PageRangeError` for invalid ``padding`` or a bad document.
    """
    if not isinstance(total_pages, int) or total_pages < 1:
        raise PageRangeError("document has no readable pages", {"total_pages": total_pages})
    if not isinstance(padding, int) or isinstance(padding, bool) or padding < 0:
        raise PageRangeError(f"padding must be a non-negative integer, got {padding!r}")

    selected: List[int] = []
    for page in pages:
        if isinstance(page, bool) or not isinstance(page, int):
            raise PageRangeError(f"page must be an integer, got {page!r}")
        if page < 1 or page > total_pages:
            raise PageRangeError(
                f"page {page} is out of range 1..{total_pages}",
                {"page": page, "total_pages": total_pages},
            )
        start = max(1, page - padding)
        end = min(total_pages, page + padding)
        selected.extend(range(start, end + 1))

    return sorted(set(selected))
