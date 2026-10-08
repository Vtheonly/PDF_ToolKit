"""Keyword search across single documents and whole corpora.

Consolidates the three original search implementations:

* the pdf-search backend (per-page match summaries, optional case
  sensitivity, substring semantics);
* the pdfgrep-based corpus scans of the original PDF Toolkit utilities
  (recursive folder discovery, per-file occurrence aggregation, ranking by
  total occurrences) - natively, without the external ``pdfgrep`` binary;
* the hardened word-boundary matching of the previous unified engine.

The unified operation keeps the strict word-boundary, case-insensitive
default and exposes both legacy behaviours through ``case_sensitive`` and
``whole_words``.
"""

from __future__ import annotations


from typing import Dict, List, Optional, Sequence, Union

from ..core.errors import PdfReadError, ValidationError
from ..core.keywords import (
    aggregate_totals,
    count_keywords,
    first_match_snippet,
    keywords_with_matches,
    normalize_keywords,
)
from ..core.models import DocumentMeta, SearchHit
from ..core.pages import PageSpec, parse_page_spec
from ..io.pdfio import open_pdf
from ..utils.files import find_pdfs
from ..utils.naming import sortable_key

from .base import Service

PathLike = Union[str, "os.PathLike"]

SORT_MODES = ("path", "rank")


class SearchService(Service):
    """Keyword search with per-page hits, snippets, match summaries and totals."""

    def search(
        self,
        reference: PathLike,
        keywords: Sequence[str],
        pages: PageSpec = None,
        case_sensitive: bool = False,
        whole_words: bool = True,
    ) -> Dict:
        """Search one registered document for the given keywords.

        The result carries both granularities the original apps produced:

        * ``hits``        - one entry per (page, keyword) with a count and a
          snippet around the first occurrence;
        * ``page_matches``- one entry per page with at least one match,
          listing which keywords were found and how many matches the page
          has in total (the pdf-search ``MatchResult`` contract).
        """
        keywords = normalize_keywords(keywords)
        meta = self.registry.resolve(reference)

        with open_pdf(meta.path) as doc:
            page_list = parse_page_spec(pages, doc.page_count)
            hits: List[SearchHit] = []
            page_matches: List[Dict] = []
            per_page: List[Dict[str, int]] = []
            for page in page_list:
                text = doc.page_text(page)
                counts = count_keywords(
                    text, keywords, case_sensitive=case_sensitive, whole_words=whole_words
                )
                per_page.append(counts)
                found = keywords_with_matches(counts)
                if found:
                    page_matches.append(
                        {
                            "page": page,
                            "count": sum(counts.values()),
                            "keywords_found": found,
                        }
                    )
                for keyword, count in counts.items():
                    if count:
                        hits.append(
                            SearchHit(
                                page=page,
                                keyword=keyword,
                                count=count,
                                snippet=first_match_snippet(
                                    text,
                                    keyword,
                                    case_sensitive=case_sensitive,
                                    whole_words=whole_words,
                                ),
                            )
                        )

        return {
            "document": meta.to_dict(),
            "keywords": keywords,
            "case_sensitive": bool(case_sensitive),
            "whole_words": bool(whole_words),
            "pages_scanned": len(page_list),
            "totals": aggregate_totals(per_page),
            "matches": len(hits),
            "matched_pages": [entry["page"] for entry in page_matches],
            "page_matches": page_matches,
            "hits": [hit.to_dict() for hit in hits],
        }

    def search_corpus(
        self,
        keywords: Sequence[str],
        directory: Optional[PathLike] = None,
        pages: PageSpec = None,
        case_sensitive: bool = False,
        whole_words: bool = True,
        recursive: bool = False,
        sort: str = "path",
    ) -> Dict:
        """Search every registered document, or every PDF in ``directory``.

        ``recursive`` also scans sub-folders (the original web-search
        engines' ``**/*.pdf`` glob behaviour). ``sort`` selects the document
        ordering of the result:

        * ``"path"`` - natural path order (default, deterministic);
        * ``"rank"`` - by total keyword occurrences, most relevant first
          (the original pdfgrep utility's ranking behaviour); ties keep
          natural path order so output stays deterministic.

        Unreadable files inside ``directory`` are skipped and reported in
        ``skipped`` rather than aborting the whole corpus scan.
        """
        keywords = normalize_keywords(keywords)
        if sort not in SORT_MODES:
            raise ValidationError(
                f"sort must be one of {SORT_MODES}, got {sort!r}", {"sort": sort}
            )

        if directory is not None:
            metas, skipped = self._scan_directory(directory, recursive=recursive)
        else:
            metas = self.registry.list()
            skipped = []

        if not metas:
            return {
                "documents_scanned": 0,
                "keywords": keywords,
                "case_sensitive": bool(case_sensitive),
                "whole_words": bool(whole_words),
                "recursive": bool(recursive),
                "sort": sort,
                "totals": {keyword: 0 for keyword in keywords},
                "documents": [],
                "skipped": skipped,
                "matches": 0,
            }

        per_document: List[Dict] = []
        totals: Dict[str, int] = {keyword: 0 for keyword in keywords}
        matches = 0
        for meta in metas:
            result = self.search(
                meta.path,
                keywords,
                pages=pages,
                case_sensitive=case_sensitive,
                whole_words=whole_words,
            )
            for keyword, count in result["totals"].items():
                totals[keyword] = totals.get(keyword, 0) + count
            matches += result["matches"]
            per_document.append(
                {
                    "document": result["document"],
                    "totals": result["totals"],
                    "total_matches": sum(result["totals"].values()),
                    "matches": result["matches"],
                    "matched_pages": result["matched_pages"],
                }
            )

        if sort == "rank":
            per_document.sort(
                key=lambda entry: (-entry["total_matches"], sortable_key(entry["document"]["path"]))
            )
            for rank, entry in enumerate(per_document, start=1):
                entry["rank"] = rank

        return {
            "documents_scanned": len(metas),
            "keywords": keywords,
            "case_sensitive": bool(case_sensitive),
            "whole_words": bool(whole_words),
            "recursive": bool(recursive),
            "sort": sort,
            "totals": totals,
            "documents": per_document,
            "skipped": skipped,
            "matches": matches,
        }

    # -- internals ----------------------------------------------------------

    def _scan_directory(self, directory: PathLike, recursive: bool = False):
        paths = find_pdfs(directory, recursive=recursive)
        metas: List[DocumentMeta] = []
        skipped: List[Dict] = []
        for path in paths:
            try:
                metas.append(self.registry.register(path))
            except PdfReadError:
                skipped.append({"path": str(path), "reason": "unreadable_pdf"})
        return metas, skipped
