"""Keyword search across single documents and whole corpora."""

from __future__ import annotations


from typing import Dict, List, Optional, Sequence, Union

from ..core.errors import PdfReadError
from ..core.keywords import (
    aggregate_totals,
    count_keywords,
    first_match_snippet,
    normalize_keywords,
)
from ..core.models import DocumentMeta, SearchHit
from ..core.pages import PageSpec, parse_page_spec
from ..io.pdfio import open_pdf
from ..utils.files import find_pdfs

from .base import Service

PathLike = Union[str, "os.PathLike"]


class SearchService(Service):
    """Keyword search with per-page hits, snippets and aggregate totals."""

    def search(
        self,
        reference: PathLike,
        keywords: Sequence[str],
        pages: PageSpec = None,
    ) -> Dict:
        """Search one registered document for the given keywords."""
        keywords = normalize_keywords(keywords)
        meta = self.registry.resolve(reference)

        with open_pdf(meta.path) as doc:
            page_list = parse_page_spec(pages, doc.page_count)
            hits: List[SearchHit] = []
            per_page: List[Dict[str, int]] = []
            for page in page_list:
                text = doc.page_text(page)
                counts = count_keywords(text, keywords)
                per_page.append(counts)
                for keyword, count in counts.items():
                    if count:
                        hits.append(
                            SearchHit(
                                page=page,
                                keyword=keyword,
                                count=count,
                                snippet=first_match_snippet(text, keyword),
                            )
                        )

        return {
            "document": meta.to_dict(),
            "keywords": keywords,
            "pages_scanned": len(page_list),
            "totals": aggregate_totals(per_page),
            "matches": len(hits),
            "hits": [hit.to_dict() for hit in hits],
        }

    def search_corpus(
        self,
        keywords: Sequence[str],
        directory: Optional[PathLike] = None,
        pages: PageSpec = None,
    ) -> Dict:
        """Search every registered document, or every PDF in ``directory``.

        Unreadable files inside ``directory`` are skipped and reported in
        ``skipped`` rather than aborting the whole corpus scan.
        """
        keywords = normalize_keywords(keywords)

        if directory is not None:
            metas, skipped = self._scan_directory(directory)
        else:
            metas = self.registry.list()
            skipped = []

        if not metas:
            return {
                "documents_scanned": 0,
                "keywords": keywords,
                "totals": {keyword: 0 for keyword in keywords},
                "documents": [],
                "skipped": skipped,
                "matches": 0,
            }

        per_document: List[Dict] = []
        totals: Dict[str, int] = {keyword: 0 for keyword in keywords}
        matches = 0
        for meta in metas:
            result = self.search(meta.path, keywords, pages=pages)
            for keyword, count in result["totals"].items():
                totals[keyword] = totals.get(keyword, 0) + count
            matches += result["matches"]
            per_document.append(
                {
                    "document": result["document"],
                    "totals": result["totals"],
                    "matches": result["matches"],
                }
            )

        return {
            "documents_scanned": len(metas),
            "keywords": keywords,
            "totals": totals,
            "documents": per_document,
            "skipped": skipped,
            "matches": matches,
        }

    # -- internals ----------------------------------------------------------

    def _scan_directory(self, directory: PathLike):
        paths = find_pdfs(directory)
        metas: List[DocumentMeta] = []
        skipped: List[Dict] = []
        for path in paths:
            try:
                metas.append(self.registry.register(path))
            except PdfReadError:
                skipped.append({"path": str(path), "reason": "unreadable_pdf"})
        return metas, skipped
