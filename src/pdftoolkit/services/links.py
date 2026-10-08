"""Hyperlink extraction service."""

from __future__ import annotations

from typing import Dict, List

from ..core.models import LinkInfo
from ..core.pages import PageSpec, parse_page_spec
from ..io.pdfio import open_pdf
from .base import Service


class LinksService(Service):
    """Collect outbound URI links from documents."""

    def extract_links(self, reference, pages: PageSpec = None) -> Dict:
        """Return every URI link on the requested pages of one document."""
        meta = self.registry.resolve(reference)

        links: List[LinkInfo] = []
        with open_pdf(meta.path) as doc:
            selected = parse_page_spec(pages, doc.page_count)
            for link in doc.extract_links(selected):
                links.append(
                    LinkInfo(
                        page=link["page"],
                        uri=link["uri"],
                        rect=tuple(link.get("rect") or (0.0, 0.0, 0.0, 0.0)),
                    )
                )
            page_count = doc.page_count

        return {
            "document": meta.to_dict(),
            "page_count": page_count,
            "count": len(links),
            "links": [link.to_dict() for link in links],
        }
