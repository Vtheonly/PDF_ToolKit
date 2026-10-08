"""Integration tests for page operations: cutting and context windows."""

import pytest

from helpers import pdf_page_texts
from pdftoolkit.core.errors import (
    ContentNotFoundError,
    OutputConflictError,
    PageRangeError,
)
from pdftoolkit.documents import DocumentRegistry
from pdftoolkit.services import PageOpsService


@pytest.fixture
def service():
    return PageOpsService(DocumentRegistry())


class TestCutPages:
    def test_cut_selection(self, service, sample_pdf, tmp_path):
        out = tmp_path / "cut.pdf"
        result = service.cut_pages(sample_pdf, "1,3", out)
        assert result["pages"] == 2
        texts = pdf_page_texts(out)
        assert len(texts) == 2
        assert "alpha beta" in texts[0]
        assert "alpha alpha" in texts[1]

    def test_cut_range_spec(self, service, sample_pdf, tmp_path):
        out = tmp_path / "cut.pdf"
        result = service.cut_pages(sample_pdf, "2-4", out)
        assert result["pages"] == 3

    def test_cut_all_pages(self, service, sample_pdf, tmp_path):
        out = tmp_path / "cut.pdf"
        result = service.cut_pages(sample_pdf, "all", out)
        assert result["pages"] == 5

    def test_output_equal_to_source_rejected(self, service, sample_pdf):
        with pytest.raises(OutputConflictError):
            service.cut_pages(sample_pdf, "1", sample_pdf)

    def test_out_of_range_pages_rejected(self, service, sample_pdf, tmp_path):
        with pytest.raises(PageRangeError):
            service.cut_pages(sample_pdf, "6", tmp_path / "cut.pdf")

    def test_malformed_spec_rejected(self, service, sample_pdf, tmp_path):
        with pytest.raises(PageRangeError):
            service.cut_pages(sample_pdf, "3-1", tmp_path / "cut.pdf")

    def test_document_block(self, service, sample_pdf, tmp_path):
        result = service.cut_pages(sample_pdf, "1", tmp_path / "cut.pdf")
        assert result["document"]["filename"] == "sample.pdf"


class TestContextPages:
    def test_keyword_anchor(self, service, sample_pdf):
        result = service.context_pages(sample_pdf, "gamma")
        assert result["anchor_page"] == 2
        assert result["pages"] == [1, 2, 3]
        assert set(result["texts"]) == {"1", "2", "3"}
        assert "gamma delta" in result["texts"]["2"]

    def test_first_match_wins(self, service, sample_pdf):
        result = service.context_pages(sample_pdf, "alpha")
        assert result["anchor_page"] == 1

    def test_page_anchor(self, service, sample_pdf):
        result = service.context_pages(sample_pdf, 4)
        assert result["anchor_page"] == 4
        assert result["pages"] == [3, 4, 5]

    def test_window_clamped_at_start(self, service, sample_pdf):
        result = service.context_pages(sample_pdf, 1, before=3, after=1)
        assert result["pages"] == [1, 2]

    def test_window_clamped_at_end(self, service, sample_pdf):
        result = service.context_pages(sample_pdf, 5, before=1, after=3)
        assert result["pages"] == [4, 5]

    def test_custom_context_size(self, service, sample_pdf):
        result = service.context_pages(sample_pdf, "epsilon", before=2, after=0)
        assert result["anchor_page"] == 4
        assert result["pages"] == [2, 3, 4]

    def test_missing_keyword_raises_content_not_found(self, service, sample_pdf):
        with pytest.raises(ContentNotFoundError) as excinfo:
            service.context_pages(sample_pdf, "unobtainium")
        assert excinfo.value.code == "content_not_found"

    def test_anchor_page_out_of_range(self, service, sample_pdf):
        with pytest.raises(PageRangeError):
            service.context_pages(sample_pdf, 99)

    def test_negative_context_rejected(self, service, sample_pdf):
        with pytest.raises(PageRangeError):
            service.context_pages(sample_pdf, 3, before=-1)

    def test_case_insensitive_anchor(self, service, sample_pdf):
        result = service.context_pages(sample_pdf, "EPSILON")
        assert result["anchor_page"] == 4

    def test_anchor_reports_original_form(self, service, sample_pdf):
        result = service.context_pages(sample_pdf, "gamma")
        assert result["anchor"] == "gamma"

    def test_page_count_reported(self, service, sample_pdf):
        assert service.context_pages(sample_pdf, 1)["page_count"] == 5


class TestExtractContext:
    def test_extracts_window_to_new_pdf(self, service, sample_pdf, tmp_path):
        out = tmp_path / "context.pdf"
        result = service.extract_context(sample_pdf, "gamma", out)
        assert result["pages"] == 3
        texts = pdf_page_texts(out)
        assert len(texts) == 3
        assert "gamma delta" in texts[1]

    def test_output_equal_to_source_rejected(self, service, sample_pdf):
        with pytest.raises(OutputConflictError):
            service.extract_context(sample_pdf, "gamma", sample_pdf)

    def test_missing_keyword_rejected(self, service, sample_pdf, tmp_path):
        with pytest.raises(ContentNotFoundError):
            service.extract_context(sample_pdf, "nope", tmp_path / "out.pdf")

    def test_anchor_page_form(self, service, sample_pdf, tmp_path):
        out = tmp_path / "context.pdf"
        result = service.extract_context(sample_pdf, 5, out)
        assert result["pages"] == 2
        assert result["anchor_page"] == 5
