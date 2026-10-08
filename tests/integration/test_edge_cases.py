"""Edge-case and regression tests for the unified engine's defensive
boundaries: encrypted PDFs, unexpected exceptions, registry lookups and
HTTP body parsing."""

import pytest

from helpers import make_pdf, pdf_page_count
from pdftoolkit import Toolkit
from pdftoolkit.core.errors import DocumentNotFoundError, PdfReadError, ValidationError
from pdftoolkit.documents import DocumentRegistry
from pdftoolkit.io.pdfio import merge_pdfs, open_pdf, select_pages
from pdftoolkit.services import SearchService


def make_encrypted_pdf(path, password="secret"):
    """Create a password-protected PDF (rejected by the engine)."""
    import fitz

    doc = fitz.open()
    page = doc.new_page(width=612, height=792)
    page.insert_text(fitz.Point(72, 96), "classified")
    doc.save(
        str(path),
        encryption=fitz.PDF_ENCRYPT_AES_256,
        owner_pw=password,
        user_pw=password,
    )
    doc.close()
    return path


class TestEncryptedPdfs:
    def test_open_encrypted_pdf_rejected(self, tmp_path):
        encrypted = make_encrypted_pdf(tmp_path / "secret.pdf")
        with pytest.raises(PdfReadError) as excinfo:
            open_pdf(encrypted)
        assert excinfo.value.code == "unreadable_pdf"
        assert "password" in excinfo.value.message

    def test_register_encrypted_pdf_rejected(self, tmp_path):
        encrypted = make_encrypted_pdf(tmp_path / "secret.pdf")
        engine = Toolkit()
        result = engine.register(encrypted)
        assert result["ok"] is False
        assert result["error"]["code"] == "unreadable_pdf"

    def test_merge_encrypted_source_rejected(self, tmp_path):
        encrypted = make_encrypted_pdf(tmp_path / "secret.pdf")
        plain = make_pdf(tmp_path / "plain.pdf", ["public"])
        with pytest.raises(PdfReadError):
            merge_pdfs([plain, encrypted], tmp_path / "out.pdf")

    def test_search_encrypted_pdf_is_failure_envelope(self, tmp_path):
        encrypted = make_encrypted_pdf(tmp_path / "secret.pdf")
        engine = Toolkit()
        result = engine.search(encrypted, ["classified"])
        assert result["ok"] is False
        assert result["error"]["code"] == "unreadable_pdf"


class TestPdfioValidationEdges:
    def test_render_page_rejects_non_positive_zoom(self, tmp_path):
        pdf = make_pdf(tmp_path / "p.pdf", ["page"])
        with open_pdf(pdf) as doc:
            with pytest.raises(ValidationError):
                doc.render_page(1, zoom=-1)

    def test_render_page_rejects_boolean_zoom(self, tmp_path):
        pdf = make_pdf(tmp_path / "p.pdf", ["page"])
        with open_pdf(pdf) as doc:
            with pytest.raises(ValidationError):
                doc.render_page(1, zoom=True)

    def test_page_text_rejects_non_integer_page(self, tmp_path):
        pdf = make_pdf(tmp_path / "p.pdf", ["page"])
        with open_pdf(pdf) as doc:
            with pytest.raises(Exception):
                doc.page_text("1")

    def test_select_pages_rejects_non_integer_page(self, tmp_path):
        pdf = make_pdf(tmp_path / "p.pdf", ["page"])
        with pytest.raises(Exception):
            select_pages(pdf, ["1"], tmp_path / "out.pdf")

    def test_select_pages_rejects_boolean_page(self, tmp_path):
        pdf = make_pdf(tmp_path / "p.pdf", ["page"])
        with pytest.raises(Exception):
            select_pages(pdf, [True], tmp_path / "out.pdf")

    def test_link_without_uri_is_skipped(self, tmp_path):
        import fitz

        pdf = tmp_path / "internal.pdf"
        doc = fitz.open()
        page = doc.new_page(width=612, height=792)
        page.insert_text(fitz.Point(72, 96), "internal navigation")
        page.insert_link(
            {"kind": fitz.LINK_GOTO, "from": fitz.Rect(72, 80, 320, 110), "page": 0}
        )
        doc.save(str(pdf), garbage=3, deflate=True)
        doc.close()
        with open_pdf(pdf) as doc2:
            assert doc2.extract_links() == []


class TestRegistryEdges:
    def test_get_unknown_id_raises(self):
        registry = DocumentRegistry()
        with pytest.raises(DocumentNotFoundError) as excinfo:
            registry.get("doc-404")
        assert excinfo.value.details == {"id": "doc-404"}

    def test_re_register_same_path_returns_same_id(self, tmp_path):
        pdf = make_pdf(tmp_path / "same.pdf", ["x"])
        registry = DocumentRegistry()
        first = registry.register(pdf)
        second = registry.register(str(pdf))  # str instead of Path
        assert first.id == second.id
        assert len(registry) == 1

    def test_register_missing_file_rejected(self, tmp_path):
        registry = DocumentRegistry()
        with pytest.raises(ValidationError):
            registry.register(tmp_path / "ghost.pdf")

    def test_resolve_none_rejected(self):
        registry = DocumentRegistry()
        with pytest.raises(ValidationError):
            registry.resolve(None)

    def test_resolve_non_pathlike_rejected(self):
        registry = DocumentRegistry()
        with pytest.raises(ValidationError):
            registry.resolve(12345)

    def test_remove_then_resolve_reregisters(self, tmp_path):
        pdf = make_pdf(tmp_path / "cycle.pdf", ["x"])
        registry = DocumentRegistry()
        first = registry.register(pdf)
        registry.remove(first.id)
        assert len(registry) == 0
        second = registry.resolve(pdf)
        assert second.id != first.id


class TestToolkitDefensiveBoundaries:
    def test_unexpected_exception_becomes_failure_envelope(self, tmp_path, monkeypatch):
        engine = Toolkit()
        pdf = make_pdf(tmp_path / "p.pdf", ["page"])

        def explode(*args, **kwargs):
            raise RuntimeError("surprise")

        monkeypatch.setattr(SearchService, "search", explode)
        result = engine.search(pdf, ["keyword"])
        assert result["ok"] is False
        assert result["error"]["code"] == "operation_failed"
        assert "surprise" in result["error"]["message"]

    def test_strict_mode_propagates_unexpected_exceptions(self, tmp_path, monkeypatch):
        engine = Toolkit(strict=True)
        pdf = make_pdf(tmp_path / "p.pdf", ["page"])

        def explode(*args, **kwargs):
            raise RuntimeError("surprise")

        monkeypatch.setattr(SearchService, "search", explode)
        with pytest.raises(RuntimeError):
            engine.search(pdf, ["keyword"])

    def test_capabilities_list_new_operations(self):
        engine = Toolkit()
        operations = engine.capabilities()["data"]["operations"]
        for expected in (
            "extract_matches",
            "render_thumbnails",
            "search_corpus",
            "merge_folder",
            "synthesize_speech",
        ):
            assert expected in operations

    def test_lazy_services_are_cached(self, speech_provider):
        engine = Toolkit(speech_provider=speech_provider)
        assert engine.search_service is engine.search_service
        assert engine.render_service is engine.render_service


class TestHttpBodyParsing:
    """The HTTP adapter must tolerate malformed JSON bodies."""

    @pytest.fixture
    def client(self, tmp_path):
        fastapi = pytest.importorskip("fastapi")
        from fastapi.testclient import TestClient

        from pdftoolkit.api.http import create_app

        return TestClient(create_app(Toolkit(strict=True), upload_dir=tmp_path / "up"))

    def test_non_json_body_reports_missing_field(self, client, sample_pdf):
        response = client.post(
            "/v1/search",
            content=b"definitely not json",
            headers={"Content-Type": "application/json"},
        )
        assert response.status_code == 400
        assert response.json()["error"]["code"] == "invalid_input"

    def test_json_array_body_reports_missing_field(self, client):
        response = client.post("/v1/search", content=b"[1, 2, 3]")
        assert response.status_code == 400
        assert response.json()["error"]["code"] == "invalid_input"

    def test_null_field_values_reported_missing(self, client):
        response = client.post(
            "/v1/search", json={"document": None, "keywords": ["alpha"]}
        )
        assert response.status_code == 400
        assert "document" in response.json()["error"]["message"]


class TestResourceCleanup:
    def test_merge_output_is_valid_pdf(self, tmp_path):
        first = make_pdf(tmp_path / "1.pdf", ["A"])
        second = make_pdf(tmp_path / "2.pdf", ["B"])
        merge_pdfs([first, second], tmp_path / "out.pdf")
        assert pdf_page_count(tmp_path / "out.pdf") == 2

    def test_failed_merge_leaves_no_output(self, tmp_path):
        good = make_pdf(tmp_path / "good.pdf", ["A"])
        encrypted = make_encrypted_pdf(tmp_path / "secret.pdf")
        out = tmp_path / "out.pdf"
        with pytest.raises(PdfReadError):
            merge_pdfs([good, encrypted], out)
        assert not out.exists()
