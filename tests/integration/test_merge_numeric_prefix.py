"""Integration tests for numeric-prefix folder merges (the original pdfm
tool: only ``N_name.pdf`` files participate, ordered by their number)."""

import pytest

from helpers import make_pdf, pdf_page_texts, write_text_file
from pdftoolkit.core.errors import ValidationError
from pdftoolkit.documents import DocumentRegistry
from pdftoolkit.services.merge import MergeService, numeric_prefix_order


@pytest.fixture
def service():
    return MergeService(DocumentRegistry())


@pytest.fixture
def pdfm_dir(tmp_path):
    """The exact folder shape the original pdfm tool was built for."""
    root = tmp_path / "pdfm"
    root.mkdir()
    make_pdf(root / "1_intro.pdf", ["intro one", "intro two"])
    make_pdf(root / "2_body.pdf", ["body one"])
    make_pdf(root / "10_appendix.pdf", ["appendix one", "appendix two", "appendix three"])
    make_pdf(root / "notes.pdf", ["plain file without numeric prefix"])
    make_pdf(root / "merged.pdf", ["stale output"])
    write_text_file(root / "5_readme.txt", "not a pdf")
    return root


class TestNumericPrefixOrder:
    def test_matching_name_returns_number(self, tmp_path):
        assert numeric_prefix_order(tmp_path / "12_chapter.pdf") == (12, "12_chapter.pdf")

    def test_plain_name_is_ignored(self, tmp_path):
        assert numeric_prefix_order(tmp_path / "chapter.pdf") is None

    def test_underscore_is_required(self, tmp_path):
        assert numeric_prefix_order(tmp_path / "12-chapter.pdf") is None

    def test_number_only_is_ignored(self, tmp_path):
        assert numeric_prefix_order(tmp_path / "12.pdf") is None

    def test_case_insensitive_suffix(self, tmp_path):
        assert numeric_prefix_order(tmp_path / "3_upper.PDF") == (3, "3_upper.PDF")

    def test_leading_zeros_preserve_value(self, tmp_path):
        assert numeric_prefix_order(tmp_path / "007_bond.pdf") == (7, "007_bond.pdf")


class TestMergeFolderNumericPrefix:
    def test_merges_only_prefixed_files_in_numeric_order(self, service, pdfm_dir):
        result = service.merge_folder(pdfm_dir, numeric_prefix=True)
        names = [path.rsplit("/", 1)[-1] for path in result["sources"]]
        assert names == ["1_intro.pdf", "2_body.pdf", "10_appendix.pdf"]
        assert result["pages"] == 6

    def test_numeric_order_not_lexicographic(self, service, pdfm_dir):
        result = service.merge_folder(pdfm_dir, numeric_prefix=True)
        texts = pdf_page_texts(pdfm_dir / "merged.pdf")
        assert "intro one" in texts[0]
        assert "body one" in texts[2]
        assert "appendix three" in texts[5]

    def test_unprefixed_files_reported_as_ignored(self, service, pdfm_dir):
        result = service.merge_folder(pdfm_dir, numeric_prefix=True)
        ignored = {entry["path"].rsplit("/", 1)[-1] for entry in result["ignored"]}
        assert ignored == {"notes.pdf"}  # merged.pdf is excluded as stale output

    def test_result_flags_numeric_prefix_mode(self, service, pdfm_dir):
        result = service.merge_folder(pdfm_dir, numeric_prefix=True)
        assert result["numeric_prefix"] is True
        plain = service.merge_folder(pdfm_dir)
        assert plain["numeric_prefix"] is False
        assert plain["files"] == 4  # notes.pdf joins when no filter is applied

    def test_default_mode_still_includes_plain_files(self, service, pdfm_dir):
        result = service.merge_folder(pdfm_dir)
        names = [path.rsplit("/", 1)[-1] for path in result["sources"]]
        assert names == ["1_intro.pdf", "2_body.pdf", "10_appendix.pdf", "notes.pdf"]

    def test_numeric_prefix_with_explicit_output(self, service, pdfm_dir, tmp_path):
        out = tmp_path / "elsewhere" / "combined.pdf"
        result = service.merge_folder(pdfm_dir, output=out, numeric_prefix=True)
        assert result["output"] == str(out)
        assert out.is_file()

    def test_idempotent_rerun(self, service, pdfm_dir):
        first = service.merge_folder(pdfm_dir, numeric_prefix=True)
        second = service.merge_folder(pdfm_dir, numeric_prefix=True)
        assert first["files"] == second["files"] == 3
        assert second["pages"] == 6

    def test_too_few_prefixed_files_rejected(self, service, tmp_path):
        root = tmp_path / "lonely"
        root.mkdir()
        make_pdf(root / "only_one.pdf", ["one"])
        with pytest.raises(ValidationError):
            service.merge_folder(root, numeric_prefix=True)

    def test_numeric_prefix_recursive(self, service, tmp_path):
        root = tmp_path / "tree"
        (root / "sub").mkdir(parents=True)
        make_pdf(root / "1_top.pdf", ["top"])
        make_pdf(root / "sub" / "2_nested.pdf", ["nested"])
        result = service.merge_folder(root, recursive=True, numeric_prefix=True)
        assert result["files"] == 2
        texts = pdf_page_texts(root / "merged.pdf")
        assert "top" in texts[0]
        assert "nested" in texts[1]
