"""Page-level operations: cutting selections, keyword-anchored context and
match-driven extraction."""

from __future__ import annotations

from typing import Dict, List, Union

from ..core.errors import ContentNotFoundError, PageRangeError
from ..core.keywords import contains_keyword, normalize_keywords
from ..core.models import DocumentMeta
from ..core.pages import PageSpec, context_window, expand_with_padding, parse_page_spec
from ..core.validation import resolve_path
from ..io.pdfio import open_pdf, select_pages
from .base import Service

Anchor = Union[int, str]


class PageOpsService(Service):
    """Cut page selections, expand keyword/page anchors and extract matches."""

    def cut_pages(self, reference, pages: PageSpec, output) -> Dict:
        """Write the selected pages of ``reference`` into a new PDF.

        ``pages`` accepts the full page-spec syntax (``"19-22,29-31"``,
        ``"1,3,5-7"``, ``"all"``), which subsumes the multi-range extraction
        plans of the original pdfContextCutter utility.
        """
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

    def extract_matches(
        self,
        reference,
        keywords,
        output,
        padding: int = 2,
        pages: PageSpec = None,
        case_sensitive: bool = False,
        whole_words: bool = True,
    ) -> Dict:
        """Search ``reference`` and extract every match plus context pages.

        This is the one-shot version of the original pdf-search workflow
        (search keywords, expand the matched pages by a ``padding`` window,
        write the union into a new PDF). It runs a single document scan and
        reuses the selection machinery, so no page is parsed twice.
        """
        keywords = normalize_keywords(keywords)
        meta = self.registry.resolve(reference)
        target = resolve_path(output, must_exist=False, kind="any")

        matched_pages: List[int] = []
        with open_pdf(meta.path) as doc:
            total = doc.page_count
            scope = parse_page_spec(pages, total)
            for page in scope:
                text = doc.page_text(page)
                if any(
                    contains_keyword(
                        text,
                        keyword,
                        case_sensitive=case_sensitive,
                        whole_words=whole_words,
                    )
                    for keyword in keywords
                ):
                    matched_pages.append(page)

        if not matched_pages:
            raise ContentNotFoundError(
                "no page in the requested scope contains any of the keywords",
                {"keywords": keywords, "pages_scanned": len(scope)},
            )

        selection = expand_with_padding(matched_pages, total, padding=padding)
        result = select_pages(meta.path, selection, target)

        return {
            "document": meta.to_dict(),
            "keywords": keywords,
            "case_sensitive": bool(case_sensitive),
            "whole_words": bool(whole_words),
            "padding": padding,
            "matched_pages": matched_pages,
            "pages_with_padding": selection,
            "pages": result["pages"],
            "page_count": total,
            "output": result["output"],
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
