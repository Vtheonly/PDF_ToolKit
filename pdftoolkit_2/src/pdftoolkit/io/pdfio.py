"""The single PDF backend of the engine.

Every PDF library concern lives here and nowhere else. The engine
standardises on **PyMuPDF** (``fitz``), which consolidates the PyMuPDF /
pypdf / PyPDF2 / pdfminer mix used by the original applications.

Public helpers accept 1-based page numbers; conversion to PyMuPDF's 0-based
indexing happens exclusively inside this module.
"""

from __future__ import annotations

from pathlib import Path
import os
from typing import Dict, Iterable, List, Sequence, Union

import fitz

from ..core.errors import (
    OperationError,
    OutputConflictError,
    PageRangeError,
    PdfReadError,
    PdfToolkitError,
)
from ..core.pages import PageSpec, parse_page_spec
from ..core.validation import resolve_path

PathLike = Union[str, os.PathLike]


def open_pdf(path: PathLike) -> "PdfDocument":
    """Open a PDF file, raising :class:`PdfReadError` for unreadable input."""
    return PdfDocument(path)


class PdfDocument:
    """Read-only view over one PDF file."""

    def __init__(self, path: PathLike) -> None:
        resolved = resolve_path(path, must_exist=True, kind="file")
        try:
            doc = fitz.open(resolved)
        except Exception as exc:  # fitz raises assorted low-level types
            raise PdfReadError(
                f"cannot open PDF: {resolved.name}", {"path": str(resolved)}
            ) from exc

        if doc.is_encrypted:
            doc.close()
            raise PdfReadError(
                f"PDF is password protected: {resolved.name}", {"path": str(resolved)}
            )

        # PyMuPDF silently "repairs" junk files into empty documents; treat
        # those as unreadable rather than as valid 0-page PDFs.
        if doc.page_count < 1:
            doc.close()
            raise PdfReadError(
                f"PDF contains no readable pages: {resolved.name}", {"path": str(resolved)}
            )

        self._doc = doc
        self.path = resolved
        self.page_count = doc.page_count

    # -- lifecycle ---------------------------------------------------------

    def close(self) -> None:
        if self._doc is not None:
            self._doc.close()
            self._doc = None

    def __enter__(self) -> "PdfDocument":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()

    # -- text --------------------------------------------------------------

    def page_text(self, page: int) -> str:
        """Extract the text of one page (1-based)."""
        index = self._validate_page(page)
        return self._doc[index].get_text("text")

    def extract_text(self, pages: PageSpec = None) -> Dict[int, str]:
        """Extract text for the requested pages as ``{page_number: text}``."""
        return {page: self.page_text(page) for page in self.list_pages(pages)}

    # -- links -------------------------------------------------------------

    def page_links(self, page: int) -> List[Dict]:
        """Return outbound URI links for one page (1-based)."""
        index = self._validate_page(page)
        links = []
        for link in self._doc[index].get_links():
            uri = link.get("uri")
            if not uri:
                continue
            rect = link.get("from", fitz.Rect(0, 0, 0, 0))
            links.append(
                {
                    "page": page,
                    "uri": uri,
                    "rect": [rect.x0, rect.y0, rect.x1, rect.y1],
                }
            )
        return links

    def extract_links(self, pages: PageSpec = None) -> List[Dict]:
        """Collect URI links across the requested pages."""
        collected: List[Dict] = []
        for page in self.list_pages(pages):
            collected.extend(self.page_links(page))
        return collected

    # -- rendering ---------------------------------------------------------

    def render_page(self, page: int, dpi: float = 150.0) -> bytes:
        """Rasterise one page (1-based) to PNG bytes at the given DPI."""
        index = self._validate_page(page)
        zoom = dpi / 72.0
        matrix = fitz.Matrix(zoom, zoom)
        pixmap = self._doc[index].get_pixmap(matrix=matrix)
        return pixmap.tobytes("png")

    # -- helpers -----------------------------------------------------------

    def list_pages(self, pages: PageSpec = None) -> List[int]:
        """Resolve a page spec against this document."""
        return parse_page_spec(pages, self.page_count)

    def _validate_page(self, page: int) -> int:
        if isinstance(page, bool) or not isinstance(page, int):
            raise PageRangeError(f"page must be an integer, got {page!r}")
        if page < 1 or page > self.page_count:
            raise PageRangeError(
                f"page {page} is out of range 1..{self.page_count}",
                {"page": page, "total_pages": self.page_count},
            )
        return page - 1


def merge_pdfs(inputs: Sequence[PathLike], output: PathLike) -> Dict:
    """Concatenate ``inputs`` (in order) into ``output``.

    Returns a summary dict. Raises :class:`OutputConflictError` when the
    output would overwrite one of its own inputs.
    """
    if len(inputs) < 2:
        raise OperationError("merge requires at least two input documents")

    sources = [resolve_path(p, must_exist=True, kind="file") for p in inputs]
    target = resolve_path(output, must_exist=False, kind="any")

    if _same_path(target, sources):
        raise OutputConflictError(
            f"output file would overwrite one of the inputs: {target}",
            {"output": str(target)},
        )

    writer = fitz.open()
    total = 0
    try:
        for source in sources:
            try:
                with fitz.open(source) as doc:
                    if doc.is_encrypted:
                        raise PdfReadError(
                            f"PDF is password protected: {source.name}",
                            {"path": str(source)},
                        )
                    if doc.page_count < 1:
                        raise PdfReadError(
                            f"PDF contains no readable pages: {source.name}",
                            {"path": str(source)},
                        )
                    writer.insert_pdf(doc)
                    total += doc.page_count
            except (PdfReadError, OutputConflictError):
                raise
            except Exception as exc:
                raise PdfReadError(
                    f"cannot read PDF: {source.name}", {"path": str(source)}
                ) from exc

        target.parent.mkdir(parents=True, exist_ok=True)
        writer.save(str(target), garbage=3, deflate=True)
    except PdfToolkitError:
        raise
    except Exception as exc:
        raise OperationError(f"merge failed: {exc}") from exc
    finally:
        writer.close()

    return {
        "output": str(target),
        "files": len(sources),
        "pages": total,
        "sources": [str(s) for s in sources],
    }


def select_pages(source: PathLike, pages: Iterable[int], output: PathLike) -> Dict:
    """Write the given 1-based ``pages`` of ``source`` into a new PDF.

    Raises :class:`OutputConflictError` when ``output`` is ``source``.
    """
    src = resolve_path(source, must_exist=True, kind="file")
    target = resolve_path(output, must_exist=False, kind="any")

    if _same_path(target, [src]):
        raise OutputConflictError(
            f"output file must differ from the source document: {target}",
            {"output": str(target)},
        )

    try:
        with fitz.open(src) as doc:
            if doc.page_count < 1:
                raise PdfReadError(
                    f"PDF contains no readable pages: {src.name}", {"path": str(src)}
                )
            wanted = parse_page_spec(list(pages), doc.page_count)
            writer = fitz.open()
            try:
                for page in wanted:
                    writer.insert_pdf(doc, from_page=page - 1, to_page=page - 1)
                target.parent.mkdir(parents=True, exist_ok=True)
                writer.save(str(target), garbage=3, deflate=True)
            finally:
                writer.close()
    except PdfToolkitError:
        raise
    except Exception as exc:
        raise OperationError(f"page selection failed: {exc}") from exc

    return {
        "output": str(target),
        "pages": len(wanted),
        "source": str(src),
    }


def _same_path(candidate: Path, others: Iterable[Path]) -> bool:
    lowered = str(candidate).lower()
    return any(str(other).lower() == lowered for other in others)
