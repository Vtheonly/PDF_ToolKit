"""Shared test helpers: tiny deterministic PDF factories and fakes.

All tests build their own documents with PyMuPDF - nothing is read from disk
fixtures - so the suite is hermetic, deterministic and fast.
"""

from __future__ import annotations

from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

import fitz


def make_pdf(
    path,
    texts: Sequence[str],
    links: Optional[Sequence[Tuple[int, str]]] = None,
) -> Path:
    """Create a PDF with one page per entry in ``texts``.

    ``links`` is an optional sequence of ``(page_number, uri)`` tuples
    (1-based) that become clickable URI links on the given pages.
    """
    doc = fitz.open()
    for text in texts:
        page = doc.new_page(width=612, height=792)
        page.insert_text(fitz.Point(72, 96), text, fontsize=16)
    for page_number, uri in links or []:
        page = doc[page_number - 1]
        rect = fitz.Rect(72, 80, 320, 110)
        page.insert_link({"kind": fitz.LINK_URI, "from": rect, "uri": uri})
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    doc.save(str(path), garbage=3, deflate=True)
    doc.close()
    return Path(path)


def make_empty_pdf(path) -> Path:
    """Create a technically valid PDF with zero pages.

    PyMuPDF (>= 1.24) refuses to *save* zero-page documents, so the file is
    written by hand: a minimal catalog/pages tree with an empty ``Kids``
    array. PyMuPDF still opens it (repairing the missing xref) and reports
    ``page_count == 0``, which the engine must reject as unreadable.
    """
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(
        b"%PDF-1.4\n"
        b"1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        b"2 0 obj\n<< /Type /Pages /Kids [] /Count 0 >>\nendobj\n"
        b"trailer\n<< /Root 1 0 R /Size 3 >>\n%%EOF\n"
    )
    return target


def make_links_pdf(path) -> Path:
    """Create a 2-page PDF with one URI link on each page."""
    return make_pdf(
        path,
        texts=["first page link", "second page link"],
        links=[(1, "https://example.com"), (2, "https://mistral.ai")],
    )


def make_corrupt_pdf(path) -> Path:
    """Create a file with a PDF header but junk content."""
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(b"%PDF-1.4\nthis is not really a pdf\n%%EOF")
    return target


def write_text_file(path, content: str) -> Path:
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(content, encoding="utf-8")
    return target


def pdf_page_count(path) -> int:
    """Open a PDF with the raw backend and return its page count."""
    with fitz.open(str(path)) as doc:
        return doc.page_count


def pdf_page_texts(path) -> List[str]:
    """Return the text of every page, in order (raw backend)."""
    with fitz.open(str(path)) as doc:
        return [page.get_text("text") for page in doc]


def keyword_totals(result: Dict) -> Dict[str, int]:
    """Extract keyword totals out of a search envelope."""
    return result["data"]["totals"]


class FakeSpeechProvider:
    """In-memory speech provider: records calls, optionally raises."""

    def __init__(self, fail_on: Optional[str] = None) -> None:
        self.saved: List[Tuple[str, str, Optional[float], Optional[float]]] = []
        self.spoken: List[Tuple[str, Optional[float], Optional[float]]] = []
        self.fail_on = fail_on

    def save(
        self,
        text: str,
        path: str,
        rate: Optional[float] = None,
        volume: Optional[float] = None,
    ) -> None:
        if self.fail_on == "save":
            raise RuntimeError("provider exploded")
        self.saved.append((text, path, rate, volume))

    def speak(
        self,
        text: str,
        rate: Optional[float] = None,
        volume: Optional[float] = None,
    ) -> None:
        if self.fail_on == "speak":
            raise RuntimeError("provider exploded")
        self.spoken.append((text, rate, volume))
