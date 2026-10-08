"""Integration tests for the one-shot extract-matches workflow (the original
pdf-search search -> padding -> export pipeline collapsed into one call)."""

import pytest

from helpers import make_pdf, pdf_page_count, pdf_page_texts
from pdftoolkit.core.errors import ContentNotFoundError, ValidationError
from pdftoolkit.documents import DocumentRegistry
from pdftoolkit.services import PageOpsService


@pytest.fixture
def service():
    return PageOpsService(DocumentRegistry())


@pytest.fixture
def haystack(tmp_path):
    return make_pdf(
        tmp_path / "haystack.pdf",
        [
            "intro text",
            "the alpha keyword appears here",
            "filler page",
            "another alpha occurrence",
            "beta section",
            "closing page",
            "alpha again near the end",
        ],
    )


class TestExtractMatches:
    def test_extracts_matches_with_default_padding(self, service, haystack, tmp_path):
        out = tmp_path / "matches.pdf"
        result = service.extract_matches(haystack, ["alpha"], out)
        assert result["matched_pages"] == [2, 4, 7]
        assert result["pages_with_padding"] == [1, 2, 3, 4, 5, 6, 7]
        assert result["pages"] == 7
        assert result["padding"] == 2
        assert result["output"] == str(out)
        assert pdf_page_count(out) == 7

    def test_padding_zero_extracts_only_matched_pages(self, service, haystack, tmp_path):
        out = tmp_path / "tight.pdf"
        result = service.extract_matches(haystack, ["alpha"], out, padding=0)
        assert result["pages_with_padding"] == [2, 4, 7]
        texts = pdf_page_texts(out)
        assert len(texts) == 3
        assert "alpha keyword" in texts[0]
        assert "another alpha" in texts[1]
        assert "alpha again" in texts[2]

    def test_overlapping_windows_merge(self, service, tmp_path):
        source = make_pdf(tmp_path / "src.pdf", ["x", "alpha", "x", "alpha", "x"])
        out = tmp_path / "merged_windows.pdf"
        result = service.extract_matches(source, ["alpha"], out, padding=2)
        assert result["pages_with_padding"] == [1, 2, 3, 4, 5]

    def test_multiple_keywords_union(self, service, haystack, tmp_path):
        out = tmp_path / "union.pdf"
        result = service.extract_matches(haystack, ["beta", "closing"], out, padding=0)
        assert result["matched_pages"] == [5, 6]
        assert pdf_page_texts(out)[0].strip() == "beta section"

    def test_page_scope_limits_the_scan(self, service, haystack, tmp_path):
        out = tmp_path / "scoped.pdf"
        result = service.extract_matches(haystack, ["alpha"], out, padding=0, pages="1-4")
        assert result["matched_pages"] == [2, 4]
        assert pdf_page_count(out) == 2

    def test_case_sensitive_search(self, service, tmp_path):
        source = make_pdf(tmp_path / "cased.pdf", ["Alpha lower", "alpha lower"])
        out = tmp_path / "cs.pdf"
        result = service.extract_matches(source, ["Alpha"], out, padding=0, case_sensitive=True)
        assert result["matched_pages"] == [1]

    def test_substring_search(self, service, tmp_path):
        source = make_pdf(tmp_path / "sub.pdf", ["alphabet soup", "nothing"])
        out = tmp_path / "sub_out.pdf"
        result = service.extract_matches(source, ["alpha"], out, padding=0, whole_words=False)
        assert result["matched_pages"] == [1]

    def test_output_conflict_rejected(self, service, haystack):
        with pytest.raises(Exception):
            service.extract_matches(haystack, ["alpha"], haystack)

    def test_result_echoes_match_options(self, service, haystack, tmp_path):
        result = service.extract_matches(
            haystack, ["alpha"], tmp_path / "echo.pdf", case_sensitive=True, whole_words=False
        )
        assert result["case_sensitive"] is True
        assert result["whole_words"] is False
        assert result["keywords"] == ["alpha"]

    def test_creates_missing_parent_directories(self, service, haystack, tmp_path):
        out = tmp_path / "deep" / "nested" / "matches.pdf"
        service.extract_matches(haystack, ["alpha"], out, padding=0)
        assert out.is_file()


class TestExtractMatchesErrors:
    def test_no_matches_raises_content_not_found(self, service, haystack, tmp_path):
        with pytest.raises(ContentNotFoundError) as excinfo:
            service.extract_matches(haystack, ["missing"], tmp_path / "none.pdf")
        assert excinfo.value.code == "content_not_found"
        assert excinfo.value.details["keywords"] == ["missing"]

    def test_no_matches_within_scope(self, service, haystack, tmp_path):
        with pytest.raises(ContentNotFoundError):
            service.extract_matches(haystack, ["alpha"], tmp_path / "none.pdf", pages="1")

    def test_empty_keywords_rejected(self, service, haystack, tmp_path):
        with pytest.raises(ValidationError):
            service.extract_matches(haystack, [], tmp_path / "none.pdf")

    def test_missing_document_rejected(self, service, tmp_path):
        with pytest.raises(Exception):
            service.extract_matches(tmp_path / "ghost.pdf", ["alpha"], tmp_path / "out.pdf")


class TestCutPagesMultiRange:
    """The pdfContextCutter extraction plans map onto page specs."""

    @pytest.fixture
    def plan_pdf(self, tmp_path):
        return make_pdf(tmp_path / "plan.pdf", [f"page {i}" for i in range(1, 21)])

    def test_multi_range_extraction_plan(self, service, plan_pdf, tmp_path):
        out = tmp_path / "bible.pdf"
        result = service.cut_pages(plan_pdf, "3-5,10,15-17", out)
        assert result["pages"] == 7
        texts = pdf_page_texts(out)
        assert texts[0].strip() == "page 3"
        assert texts[3].strip() == "page 10"
        assert texts[-1].strip() == "page 17"

    def test_single_range(self, service, plan_pdf, tmp_path):
        result = service.cut_pages(plan_pdf, "5-7", tmp_path / "range.pdf")
        assert result["pages"] == 3

    def test_reordered_selection_via_io_layer(self, plan_pdf, tmp_path):
        from pdftoolkit.io.pdfio import select_pages

        out = tmp_path / "reordered.pdf"
        select_pages(plan_pdf, [7, 2], out)
        texts = pdf_page_texts(out)
        assert texts[0].strip() == "page 7"
        assert texts[1].strip() == "page 2"
