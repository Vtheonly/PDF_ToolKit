"""Integration tests for text and link extraction services."""

import pytest

from helpers import make_pdf
from pdftoolkit.core.errors import DocumentNotFoundError, PageRangeError
from pdftoolkit.documents import DocumentRegistry
from pdftoolkit.services import LinksService, TextService


@pytest.fixture
def text_service():
    return TextService(DocumentRegistry())


@pytest.fixture
def links_service():
    return LinksService(DocumentRegistry())


class TestTextService:
    def test_extract_all_pages(self, text_service, sample_pdf):
        result = text_service.extract_text(sample_pdf)
        assert result["page_count"] == 5
        assert result["pages_requested"] == 5
        assert set(result["pages"]) == {"1", "2", "3", "4", "5"}
        assert "alpha beta" in result["pages"]["1"]

    def test_extract_selected_pages(self, text_service, sample_pdf):
        result = text_service.extract_text(sample_pdf, pages="2-3")
        assert set(result["pages"]) == {"2", "3"}
        assert "gamma delta" in result["pages"]["2"]

    def test_single_page(self, text_service, sample_pdf):
        result = text_service.extract_text(sample_pdf, pages="4")
        assert "epsilon" in result["pages"]["4"]

    def test_character_count(self, text_service, sample_pdf):
        result = text_service.extract_text(sample_pdf)
        expected = sum(len(text) for text in result["pages"].values())
        assert result["characters"] == expected

    def test_document_block(self, text_service, sample_pdf):
        result = text_service.extract_text(sample_pdf)
        assert result["document"]["page_count"] == 5

    def test_invalid_page_spec(self, text_service, sample_pdf):
        with pytest.raises(PageRangeError):
            text_service.extract_text(sample_pdf, pages="6")

    def test_unknown_document(self, text_service, tmp_path):
        with pytest.raises(DocumentNotFoundError):
            text_service.extract_text(tmp_path / "ghost.pdf")

    def test_keys_are_strings_for_json(self, text_service, sample_pdf):
        result = text_service.extract_text(sample_pdf)
        assert all(isinstance(key, str) for key in result["pages"])


class TestLinksService:
    def test_extract_all_links(self, links_service, links_pdf):
        result = links_service.extract_links(links_pdf)
        assert result["count"] == 2
        assert [link["page"] for link in result["links"]] == [1, 2]
        assert result["links"][0]["uri"] == "https://example.com"
        assert result["links"][1]["uri"] == "https://mistral.ai"

    def test_filter_by_page(self, links_service, links_pdf):
        result = links_service.extract_links(links_pdf, pages="2")
        assert result["count"] == 1
        assert result["links"][0]["page"] == 2

    def test_rect_present(self, links_service, links_pdf):
        result = links_service.extract_links(links_pdf)
        assert all(len(link["rect"]) == 4 for link in result["links"])

    def test_document_without_links(self, links_service, sample_pdf):
        result = links_service.extract_links(sample_pdf)
        assert result["count"] == 0
        assert result["links"] == []

    def test_document_block(self, links_service, links_pdf):
        result = links_service.extract_links(links_pdf)
        assert result["document"]["filename"] == "links.pdf"

    def test_unknown_document(self, links_service, tmp_path):
        with pytest.raises(DocumentNotFoundError):
            links_service.extract_links(tmp_path / "ghost.pdf")
