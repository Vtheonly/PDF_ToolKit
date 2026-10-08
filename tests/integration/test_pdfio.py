"""Integration tests for the isolated PDF backend (io/pdfio.py)."""

import pytest

from helpers import make_empty_pdf, make_links_pdf, make_pdf, pdf_page_texts
from pdftoolkit.core.errors import (
    OperationError,
    OutputConflictError,
    PageRangeError,
    PdfReadError,
)
from pdftoolkit.io.pdfio import merge_pdfs, open_pdf, select_pages

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


class TestOpenPdf:
    def test_valid_document(self, tmp_path):
        path = make_pdf(tmp_path / "ok.pdf", ["one", "two"])
        with open_pdf(path) as doc:
            assert doc.page_count == 2
            assert doc.page_text(1).strip() == "one"

    def test_first_page_is_one_based(self, tmp_path):
        path = make_pdf(tmp_path / "ok.pdf", ["first", "second"])
        with open_pdf(path) as doc:
            assert "first" in doc.page_text(1)
            assert "second" in doc.page_text(2)

    def test_missing_file(self, tmp_path):
        with pytest.raises(PdfReadError):
            open_pdf(tmp_path / "ghost.pdf")

    def test_corrupt_file(self, corrupt_pdf):
        with pytest.raises(PdfReadError) as excinfo:
            open_pdf(corrupt_pdf)
        assert excinfo.value.code == "unreadable_pdf"
        assert excinfo.value.details["path"] == str(corrupt_pdf)

    def test_zero_page_file_is_unreadable(self, tmp_path):
        empty = make_empty_pdf(tmp_path / "empty.pdf")
        with pytest.raises(PdfReadError):
            open_pdf(empty)

    def test_non_pdf_file(self, tmp_path):
        target = tmp_path / "not.pdf"
        target.write_bytes(b"just text, definitely not a pdf")
        with pytest.raises(PdfReadError):
            open_pdf(target)


class TestPdfDocumentText:
    def test_extract_all_pages(self, tmp_path):
        path = make_pdf(tmp_path / "ok.pdf", ["aa", "bb", "cc"])
        with open_pdf(path) as doc:
            texts = doc.extract_text()
        assert list(texts) == [1, 2, 3]
        assert "bb" in texts[2]

    def test_extract_selected_pages(self, tmp_path):
        path = make_pdf(tmp_path / "ok.pdf", ["aa", "bb", "cc"])
        with open_pdf(path) as doc:
            texts = doc.extract_text([3])
        assert list(texts) == [3]

    def test_page_zero_rejected(self, tmp_path):
        path = make_pdf(tmp_path / "ok.pdf", ["aa"])
        with open_pdf(path) as doc:
            with pytest.raises(PageRangeError):
                doc.page_text(0)

    def test_page_beyond_count_rejected(self, tmp_path):
        path = make_pdf(tmp_path / "ok.pdf", ["aa"])
        with open_pdf(path) as doc:
            with pytest.raises(PageRangeError):
                doc.page_text(2)

    def test_close_is_idempotent(self, tmp_path):
        path = make_pdf(tmp_path / "ok.pdf", ["aa"])
        doc = open_pdf(path)
        doc.close()
        doc.close()

    def test_context_manager_closes(self, tmp_path):
        path = make_pdf(tmp_path / "ok.pdf", ["aa"])
        doc = open_pdf(path)
        with doc:
            pass
        assert doc._doc is None


class TestPdfDocumentLinks:
    def test_extract_links(self, tmp_path):
        path = make_links_pdf(tmp_path / "links.pdf")
        with open_pdf(path) as doc:
            links = doc.extract_links()
        assert len(links) == 2
        assert links[0]["page"] == 1
        assert links[0]["uri"] == "https://example.com"
        assert links[1]["page"] == 2
        assert links[1]["uri"] == "https://mistral.ai"

    def test_links_filtered_by_pages(self, tmp_path):
        path = make_links_pdf(tmp_path / "links.pdf")
        with open_pdf(path) as doc:
            links = doc.extract_links("1")
        assert len(links) == 1
        assert links[0]["page"] == 1

    def test_rect_is_four_floats(self, tmp_path):
        path = make_links_pdf(tmp_path / "links.pdf")
        with open_pdf(path) as doc:
            link = doc.extract_links("1")[0]
        assert len(link["rect"]) == 4

    def test_document_without_links(self, tmp_path):
        path = make_pdf(tmp_path / "plain.pdf", ["no links"])
        with open_pdf(path) as doc:
            assert doc.extract_links() == []


class TestRenderPage:
    def test_returns_png_bytes(self, tmp_path):
        path = make_pdf(tmp_path / "ok.pdf", ["rendered"])
        with open_pdf(path) as doc:
            png = doc.render_page(1, zoom=1.0)
        assert png.startswith(PNG_MAGIC)

    def test_higher_dpi_produces_larger_image(self, tmp_path):
        path = make_pdf(tmp_path / "ok.pdf", ["rendered"])
        with open_pdf(path) as doc:
            small = doc.render_page(1, zoom=0.7)
            large = doc.render_page(1, zoom=2.8)
        assert len(large) > len(small)

    def test_invalid_page_rejected(self, tmp_path):
        path = make_pdf(tmp_path / "ok.pdf", ["rendered"])
        with open_pdf(path) as doc:
            with pytest.raises(PageRangeError):
                doc.render_page(9)


class TestMergePdfs:
    def test_merges_in_order(self, tmp_path):
        first = make_pdf(tmp_path / "1.pdf", ["A1"])
        second = make_pdf(tmp_path / "2.pdf", ["B1", "B2"])
        out = tmp_path / "merged.pdf"
        result = merge_pdfs([first, second], out)
        assert result["files"] == 2
        assert result["pages"] == 3
        texts = pdf_page_texts(out)
        assert ["A1" in t for t in texts] == [True, False, False]
        assert ["B2" in t for t in texts] == [False, False, True]

    def test_single_input_rejected(self, tmp_path):
        only = make_pdf(tmp_path / "only.pdf", ["A"])
        with pytest.raises(OperationError):
            merge_pdfs([only], tmp_path / "out.pdf")

    def test_empty_input_list_rejected(self, tmp_path):
        with pytest.raises(OperationError):
            merge_pdfs([], tmp_path / "out.pdf")

    def test_output_conflict_rejected(self, tmp_path):
        first = make_pdf(tmp_path / "1.pdf", ["A"])
        second = make_pdf(tmp_path / "2.pdf", ["B"])
        with pytest.raises(OutputConflictError):
            merge_pdfs([first, second], first)

    def test_corrupt_input_rejected(self, tmp_path, corrupt_pdf):
        good = make_pdf(tmp_path / "good.pdf", ["A"])
        with pytest.raises(PdfReadError):
            merge_pdfs([good, corrupt_pdf], tmp_path / "out.pdf")

    def test_creates_missing_parent_directories(self, tmp_path):
        first = make_pdf(tmp_path / "1.pdf", ["A"])
        second = make_pdf(tmp_path / "2.pdf", ["B"])
        out = tmp_path / "deep" / "nested" / "out.pdf"
        merge_pdfs([first, second], out)
        assert out.exists()


class TestSelectPages:
    def test_selects_subset_in_document_order(self, tmp_path):
        source = make_pdf(tmp_path / "src.pdf", ["p1", "p2", "p3", "p4"])
        out = tmp_path / "cut.pdf"
        result = select_pages(source, [3, 1], out)
        assert result["pages"] == 2
        texts = pdf_page_texts(out)
        assert len(texts) == 2
        assert "p3" in texts[0]
        assert "p1" in texts[1]

    def test_source_equals_output_rejected(self, tmp_path):
        source = make_pdf(tmp_path / "src.pdf", ["p1"])
        with pytest.raises(OutputConflictError):
            select_pages(source, [1], source)

    def test_out_of_range_page_rejected(self, tmp_path):
        source = make_pdf(tmp_path / "src.pdf", ["p1"])
        with pytest.raises(PageRangeError):
            select_pages(source, [2], tmp_path / "out.pdf")

    def test_empty_selection_rejected(self, tmp_path):
        source = make_pdf(tmp_path / "src.pdf", ["p1"])
        with pytest.raises(PageRangeError):
            select_pages(source, [], tmp_path / "out.pdf")

    def test_corrupt_source_rejected(self, corrupt_pdf, tmp_path):
        with pytest.raises((PdfReadError, OperationError)):
            select_pages(corrupt_pdf, [1], tmp_path / "out.pdf")
