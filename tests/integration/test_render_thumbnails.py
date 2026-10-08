"""Integration tests for thumbnail rendering (the original pdf-search
preview contract: scale factor + ``data:image/png;base64`` URIs)."""

import base64

import pytest

from helpers import make_pdf
from pdftoolkit.core.errors import ValidationError
from pdftoolkit.documents import DocumentRegistry
from pdftoolkit.services import RenderService

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
DATA_URI_PREFIX = "data:image/png;base64,"


@pytest.fixture
def service():
    return RenderService(DocumentRegistry())


@pytest.fixture
def preview_pdf(tmp_path):
    return make_pdf(tmp_path / "preview.pdf", ["one", "two", "three"])


class TestRenderThumbnails:
    def test_returns_data_uri_per_page(self, service, preview_pdf):
        result = service.render_thumbnails(preview_pdf, pages="1")
        thumb = result["thumbnails"]["1"]
        assert thumb.startswith(DATA_URI_PREFIX)
        raw = base64.b64decode(thumb[len(DATA_URI_PREFIX):])
        assert raw.startswith(PNG_MAGIC)

    def test_default_scale_is_pdf_search_default(self, service, preview_pdf):
        result = service.render_thumbnails(preview_pdf, pages="1")
        assert result["scale"] == 0.3

    def test_all_pages_by_default(self, service, preview_pdf):
        result = service.render_thumbnails(preview_pdf)
        assert result["pages_rendered"] == 3
        assert set(result["thumbnails"]) == {"1", "2", "3"}

    def test_page_selection(self, service, preview_pdf):
        result = service.render_thumbnails(preview_pdf, pages="2-3")
        assert set(result["thumbnails"]) == {"2", "3"}

    def test_larger_scale_produces_larger_payload(self, service, preview_pdf):
        small = service.render_thumbnails(preview_pdf, pages="1", scale=0.2)
        large = service.render_thumbnails(preview_pdf, pages="1", scale=1.0)
        assert len(large["thumbnails"]["1"]) > len(small["thumbnails"]["1"])

    def test_thumbnail_is_smaller_than_full_render(self, service, preview_pdf):
        thumb = service.render_thumbnails(preview_pdf, pages="1", scale=0.3)
        full = service.render_pages(preview_pdf, pages="1", dpi=150)
        assert len(thumb["thumbnails"]["1"]) < len(full["images"]["1"])

    def test_result_reports_document_and_scale(self, service, preview_pdf):
        result = service.render_thumbnails(preview_pdf, pages="1", scale=0.5)
        assert result["document"]["filename"] == "preview.pdf"
        assert result["scale"] == 0.5
        assert result["page_count"] == 3

    @pytest.mark.parametrize("scale", [0, -0.5, 10, "big", None])
    def test_invalid_scale_rejected(self, service, preview_pdf, scale):
        with pytest.raises(ValidationError):
            service.render_thumbnails(preview_pdf, scale=scale)

    def test_invalid_pages_rejected(self, service, preview_pdf):
        with pytest.raises(Exception):
            service.render_thumbnails(preview_pdf, pages="99")

    def test_original_server_default_workflow(self, service, preview_pdf):
        """The original server rendered previews of padded search results."""
        search_pages = [2]
        padded = [1, 2, 3]
        result = service.render_thumbnails(preview_pdf, pages=padded)
        assert set(result["thumbnails"]) == {"1", "2", "3"}
        assert all(
            thumb.startswith(DATA_URI_PREFIX) for thumb in result["thumbnails"].values()
        )
