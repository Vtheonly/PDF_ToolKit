"""Text extraction service."""

from __future__ import annotations

from typing import Dict

from ..core.pages import PageSpec
from ..io.pdfio import open_pdf
from .base import Service


class TextService(Service):
    """Extract page text, whole documents or specific page selections."""

    def extract_text(self, reference, pages: PageSpec = None) -> Dict:
        """Return the text of the requested pages of one document."""
        meta = self.registry.resolve(reference)

        with open_pdf(meta.path) as doc:
            selected = parse_page_spec(pages, doc.page_count)
            texts = doc.extract_text(selected)
            page_count = doc.page_count

        return {
            "document": meta.to_dict(),
            "page_count": page_count,
            "pages_requested": len(selected),
            "characters": sum(len(text) for text in texts.values()),
            "pages": {str(page): text for page, text in texts.items()},
        }
