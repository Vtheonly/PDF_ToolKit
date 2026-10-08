"""Page-level operations: cutting selections and keyword-anchored context."""

from __future__ import annotations

from typing import Dict, List, Union

from ..core.errors import ContentNotFoundError, PageRangeError
from ..core.keywords import contains_keyword
from ..core.models import DocumentMeta
from ..core.pages import PageSpec, context_window, parse_page_spec
from ..core.validation import resolve_path
from ..io.pdfio import open_pdf, select_pages
from .base import Service

Anchor = Union[int, str]


class PageOpsService(Service):
    """Cut page selections and expand keyword/page anchors to context."""

    def cut_pages(self, reference, pages: PageSpec, output) -> Dict:
        """Write the selected pages of ``reference`` into a new PDF."""
        meta = self.registry.resolve(reference)
        target = resolve_path(output, must_exist=False, kind="any")

        result = select_pages(meta.path, parse_page_spec(pages, self.registry.page_count(meta.id)), target)
        return {
            "document": meta.to_dict(),
            "output": result["output"],
            "pages": result["pages"],
        }

    def context_pages(
        self,
        reference,
        anchor: Anchor,
        before: int = 1,
        after: int = 1,
    ) -> Dict:
        """Return the pages surrounding an anchor page or keyword match.

        ``anchor`` is either a 1-based page number or a keyword; for a
        keyword the first page containing it (case-insensitive, whole word)
        becomes the anchor.
        """
        meta = self.registry.resolve(reference)

        with open_pdf(meta.path) as doc:
            total = doc.page_count
            anchor_page = self._anchor_page(doc, anchor)
            window = context_window(anchor_page, total, before=before, after=after)
            texts = {str(page): doc.page_text(page) for page in window}

        return {
            "document": meta.to_dict(),
            "anchor": anchor if isinstance(anchor, str) else int(anchor),
            "anchor_page": anchor_page,
            "before": before,
            "after": after,
            "pages": window,
            "page_count": total,
            "texts": texts,
        }

    def extract_context(
        self,
        reference,
        anchor: Anchor,
        output,
        before: int = 1,
        after: int = 1,
    ) -> Dict:
        """Extract the context window around an anchor into a new PDF."""
        context = self.context_pages(reference, anchor, before=before, after=after)
        meta: DocumentMeta = self.registry.resolve(reference)
        result = select_pages(meta.path, context["pages"], output)

        return {
            "document": meta.to_dict(),
            "anchor": context["anchor"],
            "anchor_page": context["anchor_page"],
            "output": result["output"],
            "pages": result["pages"],
        }

    # -- internals ----------------------------------------------------------

    @staticmethod
    def _anchor_page(doc, anchor: Anchor) -> int:
        if isinstance(anchor, bool):
            raise PageRangeError(f"invalid anchor: {anchor!r}")
        if isinstance(anchor, int):
            if anchor < 1 or anchor > doc.page_count:
                raise PageRangeError(
                    f"anchor page {anchor} is out of range 1..{doc.page_count}",
                    {"anchor": anchor, "total_pages": doc.page_count},
                )
            return anchor

        keyword = str(anchor).strip()
        if not keyword:
            raise PageRangeError("anchor keyword must not be empty")

        for page in range(1, doc.page_count + 1):
            if contains_keyword(doc.page_text(page), keyword):
                return page

        raise ContentNotFoundError(
            f"keyword not found in document: {keyword}", {"keyword": keyword}
        )
