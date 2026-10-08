"""Integration tests for the merge service, including folder merges."""

import os

import pytest

from helpers import make_pdf, pdf_page_texts
from pdftoolkit.core.errors import OutputConflictError, PdfReadError, ValidationError
from pdftoolkit.documents import DocumentRegistry
from pdftoolkit.services import MergeService


@pytest.fixture
def service():
    return MergeService(DocumentRegistry())


class TestMerge:
    def test_merges_in_given_order(self, service, tmp_path):
        first = make_pdf(tmp_path / "1.pdf", ["A"])
        second = make_pdf(tmp_path / "2.pdf", ["B"])
        out = tmp_path / "out.pdf"
        result = service.merge([first, second], out)
        assert result["files"] == 2
        assert result["pages"] == 2
        texts = pdf_page_texts(out)
        assert "A" in texts[0]
        assert "B" in texts[1]

    def test_at_least_two_inputs_required(self, service, tmp_path):
        only = make_pdf(tmp_path / "only.pdf", ["A"])
        with pytest.raises(ValidationError):
            service.merge([only], tmp_path / "out.pdf")

    def test_empty_input_list_rejected(self, service, tmp_path):
        with pytest.raises(ValidationError):
            service.merge([], tmp_path / "out.pdf")

    def test_none_inputs_rejected(self, service, tmp_path):
        with pytest.raises(ValidationError):
            service.merge(None, tmp_path / "out.pdf")

    def test_output_conflict_rejected(self, service, tmp_path):
        first = make_pdf(tmp_path / "1.pdf", ["A"])
        second = make_pdf(tmp_path / "2.pdf", ["B"])
        with pytest.raises(OutputConflictError):
            service.merge([first, second], first)

    def test_corrupt_input_rejected(self, service, tmp_path, corrupt_pdf):
        good = make_pdf(tmp_path / "good.pdf", ["A"])
        with pytest.raises(PdfReadError):
            service.merge([good, corrupt_pdf], tmp_path / "out.pdf")

    def test_missing_input_rejected(self, service, tmp_path):
        good = make_pdf(tmp_path / "good.pdf", ["A"])
        with pytest.raises(ValidationError):
            service.merge([good, tmp_path / "ghost.pdf"], tmp_path / "out.pdf")


class TestMergeFolder:
    def test_natural_order(self, service, merge_dir):
        result = service.merge_folder(merge_dir)
        out = merge_dir / "merged.pdf"
        assert result["output"] == str(out)
        assert result["files"] == 3
        names = [os.path.basename(p) for p in result["sources"]]
        assert names == ["1_alpha.pdf", "2_beta.pdf", "10_zeta.pdf"]

    def test_page_totals(self, service, merge_dir):
        result = service.merge_folder(merge_dir)
        # 1_alpha has 2 pages, 2_beta 1, 10_zeta 3
        assert result["pages"] == 6

    def test_merged_content_follows_natural_order(self, service, merge_dir):
        service.merge_folder(merge_dir)
        texts = pdf_page_texts(merge_dir / "merged.pdf")
        assert "alpha one" in texts[0]
        assert "beta one" in texts[2]
        assert "zeta three" in texts[5]

    def test_default_output_excluded_from_candidates(self, service, merge_dir):
        """Rerunning merge_folder must stay idempotent."""
        first = service.merge_folder(merge_dir)
        second = service.merge_folder(merge_dir)
        assert first["files"] == second["files"] == 3
        assert second["pages"] == 6

    def test_explicit_output_conflict_rejected(self, service, merge_dir):
        target = merge_dir / "1_alpha.pdf"
        with pytest.raises(OutputConflictError):
            service.merge_folder(merge_dir, output=target)

    def test_explicit_output_elsewhere_allowed(self, service, merge_dir, tmp_path):
        out = tmp_path / "elsewhere" / "combined.pdf"
        result = service.merge_folder(merge_dir, output=out)
        assert result["output"] == str(out)
        assert out.exists()

    def test_non_pdf_files_ignored(self, service, merge_dir):
        result = service.merge_folder(merge_dir)
        assert result["files"] == 3
        assert all(p.endswith(".pdf") for p in result["sources"])

    def test_unreadable_pdfs_skipped(self, service, tmp_path):
        from helpers import make_corrupt_pdf

        root = tmp_path / "mixed"
        root.mkdir()
        make_pdf(root / "1_one.pdf", ["one"])
        make_pdf(root / "2_two.pdf", ["two"])
        make_corrupt_pdf(root / "3_broken.pdf")
        result = service.merge_folder(root)
        assert result["files"] == 2
        assert result["skipped"] == [
            {"path": str(root / "3_broken.pdf"), "reason": "unreadable_pdf"}
        ]

    def test_too_few_readable_pdfs_rejected(self, tmp_path):
        service = MergeService(DocumentRegistry())
        root = tmp_path / "lonely"
        root.mkdir()
        make_pdf(root / "only.pdf", ["one"])
        with pytest.raises(ValidationError):
            service.merge_folder(root)

    def test_missing_directory_rejected(self, service, tmp_path):
        with pytest.raises(ValidationError):
            service.merge_folder(tmp_path / "ghost")

    def test_recursive_merge(self, service, tmp_path):
        root = tmp_path / "tree"
        (root / "sub").mkdir(parents=True)
        make_pdf(root / "1_top.pdf", ["top"])
        make_pdf(root / "sub" / "2_nested.pdf", ["nested"])
        result = service.merge_folder(root, recursive=True)
        assert result["files"] == 2
        assert result["recursive"] is True

    def test_pages_per_source_reported(self, service, merge_dir):
        result = service.merge_folder(merge_dir)
        pages = {entry["pages"] for entry in result["pages_per_source"]}
        assert pages == {2, 1, 3}

    def test_output_inside_other_directory(self, service, merge_dir, tmp_path):
        out = tmp_path / "nested" / "deep" / "result.pdf"
        result = service.merge_folder(merge_dir, output=out)
        assert result["pages"] == 6
        assert out.exists()
