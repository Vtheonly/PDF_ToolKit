"""Integration tests for the thread-safe document registry."""

import pytest

from helpers import make_pdf
from pdftoolkit.core.errors import DocumentNotFoundError, PdfReadError, ValidationError
from pdftoolkit.documents import DocumentRegistry


@pytest.fixture
def registry():
    return DocumentRegistry()


class TestRegister:
    def test_returns_stable_sequential_ids(self, registry, tmp_path):
        first = make_pdf(tmp_path / "one.pdf", ["a"])
        second = make_pdf(tmp_path / "two.pdf", ["b"])
        assert registry.register(first).id == "doc-1"
        assert registry.register(second).id == "doc-2"

    def test_meta_contains_path_and_count(self, registry, tmp_path):
        path = make_pdf(tmp_path / "doc.pdf", ["a", "b", "c"])
        meta = registry.register(path)
        assert meta.filename == "doc.pdf"
        assert meta.page_count == 3
        assert meta.path == str(path.resolve())

    def test_re_registering_same_path_returns_same_entry(self, registry, tmp_path):
        path = make_pdf(tmp_path / "doc.pdf", ["a"])
        first = registry.register(path)
        second = registry.register(str(path))
        assert first.id == second.id

    def test_corrupt_pdf_rejected(self, registry, corrupt_pdf):
        with pytest.raises(PdfReadError):
            registry.register(corrupt_pdf)
        assert len(registry) == 0

    def test_registration_is_atomic_on_failure(self, registry, tmp_path):
        good = make_pdf(tmp_path / "good.pdf", ["a"])
        registry.register(good)
        broken = tmp_path / "broken.pdf"
        broken.write_bytes(b"junk")
        with pytest.raises(PdfReadError):
            registry.register(broken)
        assert len(registry) == 1
        assert [m.id for m in registry.list()] == ["doc-1"]

    def test_missing_file_rejected(self, registry, tmp_path):
        with pytest.raises(ValidationError):
            registry.register(tmp_path / "ghost.pdf")

    def test_directory_rejected(self, registry, tmp_path):
        (tmp_path / "dir").mkdir()
        with pytest.raises(ValidationError):
            registry.register(tmp_path / "dir")


class TestResolve:
    def test_resolve_by_id(self, registry, tmp_path):
        path = make_pdf(tmp_path / "doc.pdf", ["a"])
        meta = registry.register(path)
        assert registry.resolve(meta.id).id == meta.id

    def test_resolve_by_path_autoregisters(self, registry, tmp_path):
        path = make_pdf(tmp_path / "doc.pdf", ["a"])
        meta = registry.resolve(path)
        assert meta.id == "doc-1"
        assert len(registry) == 1

    def test_resolve_by_pathlike(self, registry, tmp_path):
        path = make_pdf(tmp_path / "doc.pdf", ["a"])
        assert registry.resolve(path).filename == "doc.pdf"

    def test_resolve_unknown_id(self, registry):
        with pytest.raises(DocumentNotFoundError):
            registry.resolve("doc-99")

    def test_resolve_missing_path(self, registry, tmp_path):
        with pytest.raises(DocumentNotFoundError):
            registry.resolve(tmp_path / "ghost.pdf")

    def test_resolve_none_rejected(self, registry):
        with pytest.raises(ValidationError):
            registry.resolve(None)

    def test_resolve_non_pathlike_rejected(self, registry):
        with pytest.raises(ValidationError):
            registry.resolve(42)

    def test_resolve_corrupt_path_raises(self, registry, corrupt_pdf):
        with pytest.raises(PdfReadError):
            registry.resolve(corrupt_pdf)


class TestQueries:
    def test_list_is_ordered_by_id(self, registry, tmp_path):
        for index in range(4):
            make_pdf(tmp_path / f"doc{index}.pdf", ["x"])
            registry.register(tmp_path / f"doc{index}.pdf")
        assert [m.id for m in registry.list()] == [
            "doc-1",
            "doc-2",
            "doc-3",
            "doc-4",
        ]

    def test_page_count_via_reference(self, registry, tmp_path):
        path = make_pdf(tmp_path / "doc.pdf", ["a", "b"])
        registry.register(path)
        assert registry.page_count(path) == 2
        assert registry.page_count("doc-1") == 2

    def test_info_returns_json_dict(self, registry, tmp_path):
        path = make_pdf(tmp_path / "doc.pdf", ["a"])
        registry.register(path)
        info = registry.info("doc-1")
        assert info["id"] == "doc-1"
        assert info["filename"] == "doc.pdf"
        assert info["page_count"] == 1

    def test_contains_by_id_and_path(self, registry, tmp_path):
        path = make_pdf(tmp_path / "doc.pdf", ["a"])
        meta = registry.register(path)
        assert meta.id in registry
        assert path in registry

    def test_not_contains_unknown(self, registry, tmp_path):
        assert "doc-404" not in registry
        assert (tmp_path / "ghost.pdf") not in registry


class TestLifecycle:
    def test_remove_by_id(self, registry, tmp_path):
        path = make_pdf(tmp_path / "doc.pdf", ["a"])
        meta = registry.register(path)
        removed = registry.remove(meta.id)
        assert removed.id == meta.id
        assert len(registry) == 0

    def test_remove_by_path(self, registry, tmp_path):
        path = make_pdf(tmp_path / "doc.pdf", ["a"])
        registry.register(path)
        registry.remove(path)
        assert len(registry) == 0

    def test_remove_allows_reregistration_with_new_id(self, registry, tmp_path):
        path = make_pdf(tmp_path / "doc.pdf", ["a"])
        first = registry.register(path)
        registry.remove(first.id)
        second = registry.register(path)
        assert second.id == "doc-2"

    def test_remove_unknown_raises(self, registry):
        with pytest.raises(DocumentNotFoundError):
            registry.remove("doc-99")

    def test_clear(self, registry, tmp_path):
        make_pdf(tmp_path / "a.pdf", ["a"])
        make_pdf(tmp_path / "b.pdf", ["b"])
        registry.register(tmp_path / "a.pdf")
        registry.register(tmp_path / "b.pdf")
        registry.clear()
        assert len(registry) == 0

    def test_len_tracks_registrations(self, registry, tmp_path):
        assert len(registry) == 0
        make_pdf(tmp_path / "a.pdf", ["a"])
        registry.register(tmp_path / "a.pdf")
        assert len(registry) == 1


class TestThreadSafety:
    def test_concurrent_registrations_are_unique(self, registry, tmp_path):
        import threading

        paths = []
        for index in range(10):
            paths.append(make_pdf(tmp_path / f"doc{index}.pdf", ["x"]))

        results = []
        lock = threading.Lock()

        def worker(path):
            meta = registry.register(path)
            with lock:
                results.append(meta.id)

        threads = [threading.Thread(target=worker, args=(p,)) for p in paths]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()

        assert sorted(results) == sorted(set(results))
        assert len(registry) == 10
