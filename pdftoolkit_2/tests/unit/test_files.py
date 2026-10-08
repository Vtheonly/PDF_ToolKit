"""Unit tests for filesystem helpers."""

import pytest

from helpers import write_text_file
from pdftoolkit.core.errors import ValidationError
from pdftoolkit.utils.files import (
    ensure_parent,
    find_pdfs,
    next_available_path,
    read_text_file,
)
from helpers import make_pdf


class TestFindPdfs:
    def test_only_pdf_files_returned(self, tmp_path):
        make_pdf(tmp_path / "a.pdf", ["a"])
        write_text_file(tmp_path / "b.txt", "nope")
        found = find_pdfs(tmp_path)
        assert [p.name for p in found] == ["a.pdf"]

    def test_extension_case_insensitive(self, tmp_path):
        make_pdf(tmp_path / "a.pdf", ["a"])
        (tmp_path / "B.PDF").write_bytes((tmp_path / "a.pdf").read_bytes())
        assert len(find_pdfs(tmp_path)) == 2

    def test_natural_order(self, tmp_path):
        for name in ("10.pdf", "2.pdf", "1.pdf"):
            make_pdf(tmp_path / name, ["x"])
        assert [p.name for p in find_pdfs(tmp_path)] == ["1.pdf", "2.pdf", "10.pdf"]

    def test_exclude_paths(self, tmp_path):
        make_pdf(tmp_path / "a.pdf", ["a"])
        make_pdf(tmp_path / "b.pdf", ["b"])
        found = find_pdfs(tmp_path, exclude=[tmp_path / "b.pdf"])
        assert [p.name for p in found] == ["a.pdf"]

    def test_non_recursive_by_default(self, tmp_path):
        make_pdf(tmp_path / "top.pdf", ["t"])
        sub = tmp_path / "sub"
        sub.mkdir()
        make_pdf(sub / "deep.pdf", ["d"])
        assert [p.name for p in find_pdfs(tmp_path)] == ["top.pdf"]

    def test_recursive_finds_nested(self, tmp_path):
        make_pdf(tmp_path / "top.pdf", ["t"])
        sub = tmp_path / "sub"
        sub.mkdir()
        make_pdf(sub / "deep.pdf", ["d"])
        found = find_pdfs(tmp_path, recursive=True)
        assert {p.name for p in found} == {"top.pdf", "deep.pdf"}

    def test_missing_directory_rejected(self, tmp_path):
        with pytest.raises(ValidationError):
            find_pdfs(tmp_path / "ghost")

    def test_file_rejected_as_directory(self, tmp_path):
        target = tmp_path / "file.pdf"
        make_pdf(target, ["x"])
        with pytest.raises(ValidationError):
            find_pdfs(target)

    def test_empty_directory(self, tmp_path):
        assert find_pdfs(tmp_path) == []


class TestNextAvailablePath:
    def test_nonexistent_returned_unchanged(self, tmp_path):
        candidate = tmp_path / "new.pdf"
        assert next_available_path(candidate) == candidate

    def test_existing_gets_counter_suffix(self, tmp_path):
        existing = tmp_path / "doc.pdf"
        existing.write_bytes(b"x")
        assert next_available_path(existing) == tmp_path / "doc(1).pdf"

    def test_counter_increments(self, tmp_path):
        (tmp_path / "doc.pdf").write_bytes(b"x")
        (tmp_path / "doc(1).pdf").write_bytes(b"x")
        assert next_available_path(tmp_path / "doc.pdf") == tmp_path / "doc(2).pdf"

    def test_preserves_suffix(self, tmp_path):
        existing = tmp_path / "img.png"
        existing.write_bytes(b"x")
        assert next_available_path(existing).suffix == ".png"

    def test_string_input(self, tmp_path):
        existing = str(tmp_path / "doc.pdf")
        open(existing, "w").close()
        assert next_available_path(existing) == tmp_path / "doc(1).pdf"


class TestEnsureParent:
    def test_creates_nested_directories(self, tmp_path):
        target = tmp_path / "a" / "b" / "out.pdf"
        parent = ensure_parent(target)
        assert parent.is_dir()
        assert parent == target.parent

    def test_existing_parent_is_idempotent(self, tmp_path):
        parent = ensure_parent(tmp_path / "x.pdf")
        assert parent == tmp_path


class TestReadTextFile:
    def test_roundtrip(self, tmp_path):
        target = write_text_file(tmp_path / "note.txt", "hello engine")
        assert read_text_file(target) == "hello engine"

    def test_missing_file_rejected(self, tmp_path):
        with pytest.raises(ValidationError):
            read_text_file(tmp_path / "ghost.txt")

    def test_directory_rejected(self, tmp_path):
        (tmp_path / "adir").mkdir()
        with pytest.raises(ValidationError):
            read_text_file(tmp_path / "adir")

    def test_binary_content_rejected(self, tmp_path):
        target = tmp_path / "binary.txt"
        target.write_bytes(b"\xff\xfe\x00invalid")
        with pytest.raises(ValidationError):
            read_text_file(target)
