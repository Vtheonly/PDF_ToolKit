"""HTTP adapter tests for the upload/download/cleanup workflow that
preserves the original pdf-search Flask server's stateless capabilities."""

import base64

import pytest

fastapi = pytest.importorskip("fastapi")  # noqa: F841
from fastapi.testclient import TestClient  # noqa: E402

from helpers import make_pdf  # noqa: E402
from pdftoolkit import Toolkit  # noqa: E402
from pdftoolkit.api.http import create_app  # noqa: E402


@pytest.fixture
def upload_dir(tmp_path):
    directory = tmp_path / "uploads"
    directory.mkdir()
    return directory


@pytest.fixture
def client(upload_dir):
    app = create_app(Toolkit(strict=True), upload_dir=upload_dir)
    return TestClient(app)


def upload(client, path, name=None):
    with open(path, "rb") as handle:
        return client.post(
            "/v1/documents/upload",
            files={"file": (name or path.name, handle, "application/pdf")},
        )


class TestUpload:
    def test_upload_registers_document(self, client, sample_pdf, upload_dir):
        response = upload(client, sample_pdf)
        assert response.status_code == 200
        data = response.json()["data"]
        assert data["document"]["page_count"] == 5
        assert data["size_bytes"] > 0
        assert data["original_name"] == "sample.pdf"
        assert data["stored_path"].startswith(str(upload_dir))

    def test_uploaded_file_lives_in_workspace(self, client, sample_pdf, upload_dir):
        from pathlib import Path as _Path

        response = upload(client, sample_pdf)
        stored = _Path(response.json()["data"]["stored_path"])
        assert stored.parent == upload_dir

    def test_upload_is_searchable_by_document_id(self, client, sample_pdf):
        doc_id = upload(client, sample_pdf).json()["data"]["document"]["id"]
        response = client.post(
            "/v1/search", json={"document": doc_id, "keywords": ["alpha"]}
        )
        assert response.status_code == 200
        assert response.json()["data"]["totals"] == {"alpha": 3}

    def test_upload_rejects_non_pdf_extension(self, client, tmp_path):
        text_file = tmp_path / "notes.txt"
        text_file.write_text("not a pdf")
        response = upload(client, text_file)
        assert response.status_code == 400
        assert response.json()["error"]["code"] == "invalid_input"

    def test_upload_rejects_corrupt_pdf(self, client, corrupt_pdf):
        response = upload(client, corrupt_pdf)
        assert response.status_code == 400
        assert response.json()["error"]["code"] == "unreadable_pdf"

    def test_upload_sanitises_dangerous_filenames(self, client, sample_pdf, upload_dir):
        response = upload(client, sample_pdf, name="../../../../etc/passwd.pdf")
        assert response.status_code == 200
        stored = response.json()["data"]["stored_path"]
        assert stored.startswith(str(upload_dir))
        assert ".." not in stored

    def test_oversized_upload_rejected(self, client, sample_pdf, tmp_path, upload_dir):
        app = create_app(Toolkit(strict=True), upload_dir=upload_dir, max_upload_bytes=100)
        tiny_client = TestClient(app)
        response = upload(tiny_client, sample_pdf)
        assert response.status_code == 400
        assert "maximum" in response.json()["error"]["message"]

    def test_failed_upload_leaves_no_file_behind(self, client, corrupt_pdf, upload_dir):
        upload(client, corrupt_pdf)
        assert list(upload_dir.iterdir()) == []


class TestDownload:
    def test_download_serves_generated_output(self, client, sample_pdf, upload_dir):
        doc_id = upload(client, sample_pdf).json()["data"]["document"]["id"]
        output = upload_dir / "filtered.pdf"
        response = client.post(
            "/v1/pages/extract-matches",
            json={"document": doc_id, "keywords": ["alpha"], "output": str(output)},
        )
        assert response.status_code == 200
        download = client.get("/v1/files", params={"path": str(output)})
        assert download.status_code == 200
        assert download.content[:4] == b"%PDF"
        assert download.headers["content-type"] == "application/pdf"

    def test_download_rejects_paths_outside_workspace(self, client, sample_pdf):
        response = client.get("/v1/files", params={"path": str(sample_pdf)})
        assert response.status_code == 403
        assert response.json()["error"]["code"] == "invalid_input"

    def test_download_rejects_traversal_attempts(self, client, upload_dir):
        response = client.get(
            "/v1/files", params={"path": str(upload_dir / ".." / "secrets.pdf")}
        )
        assert response.status_code in (403, 404)

    def test_download_missing_file_returns_404(self, client, upload_dir):
        response = client.get(
            "/v1/files", params={"path": str(upload_dir / "ghost.pdf")}
        )
        assert response.status_code == 404

    def test_download_requires_path_parameter(self, client):
        response = client.get("/v1/files")
        assert response.status_code in (400, 422)


class TestCleanup:
    def test_delete_deregisters_document(self, client, sample_pdf):
        doc_id = upload(client, sample_pdf).json()["data"]["document"]["id"]
        response = client.delete(f"/v1/documents/{doc_id}")
        assert response.status_code == 200
        listing = client.get("/v1/documents").json()["data"]["documents"]
        assert all(doc["id"] != doc_id for doc in listing)

    def test_delete_without_files_keeps_upload_on_disk(self, client, sample_pdf):
        data = upload(client, sample_pdf).json()["data"]
        client.delete(f"/v1/documents/{data['document']['id']}")
        import pathlib

        assert pathlib.Path(data["stored_path"]).is_file()

    def test_delete_with_files_removes_upload(self, client, sample_pdf):
        data = upload(client, sample_pdf).json()["data"]
        response = client.delete(
            f"/v1/documents/{data['document']['id']}", params={"delete_files": "true"}
        )
        assert response.status_code == 200
        assert response.json()["data"]["deleted_file"] == data["stored_path"]
        import pathlib

        assert not pathlib.Path(data["stored_path"]).exists()

    def test_delete_with_files_never_touches_user_files(self, client, sample_pdf):
        """Files outside the workspace are never deleted by the adapter."""
        doc_id = client.post(
            "/v1/documents/register", json={"path": str(sample_pdf)}
        ).json()["data"]["document"]["id"]
        response = client.delete(
            f"/v1/documents/{doc_id}", params={"delete_files": "true"}
        )
        assert response.status_code == 200
        assert "deleted_file" not in response.json()["data"]
        assert sample_pdf.is_file()

    def test_full_upload_search_export_cleanup_workflow(self, client, sample_pdf, upload_dir):
        """The complete original pdf-search server workflow, end to end."""
        # 1. upload
        doc_id = upload(client, sample_pdf).json()["data"]["document"]["id"]
        # 2. search
        search = client.post(
            "/v1/search", json={"document": doc_id, "keywords": ["alpha"]}
        ).json()["data"]
        assert search["matched_pages"] == [1, 3]
        # 3. export the padded selection
        output = upload_dir / "filtered_output.pdf"
        export = client.post(
            "/v1/pages/extract-matches",
            json={
                "document": doc_id,
                "keywords": ["alpha"],
                "output": str(output),
                "padding": 2,
            },
        )
        assert export.status_code == 200
        # 4. download the export
        download = client.get("/v1/files", params={"path": str(output)})
        assert download.status_code == 200
        # 5. cleanup
        cleanup = client.delete(
            f"/v1/documents/{doc_id}", params={"delete_files": "true"}
        )
        assert cleanup.status_code == 200


class TestNewOperationEndpoints:
    def test_thumbnails_endpoint(self, client, sample_pdf):
        doc_id = upload(client, sample_pdf).json()["data"]["document"]["id"]
        response = client.post(
            "/v1/render/thumbnails", json={"document": doc_id, "pages": "1", "scale": 0.3}
        )
        assert response.status_code == 200
        thumb = response.json()["data"]["thumbnails"]["1"]
        assert thumb.startswith("data:image/png;base64,")
        assert base64.b64decode(thumb.split(",", 1)[1])[:4] == b"\x89PNG"

    def test_extract_matches_endpoint(self, client, sample_pdf, upload_dir):
        doc_id = upload(client, sample_pdf).json()["data"]["document"]["id"]
        output = upload_dir / "matches.pdf"
        response = client.post(
            "/v1/pages/extract-matches",
            json={
                "document": doc_id,
                "keywords": ["beta"],
                "output": str(output),
                "padding": 1,
            },
        )
        assert response.status_code == 200
        data = response.json()["data"]
        assert data["matched_pages"] == [1, 5]
        assert data["pages_with_padding"] == [1, 2, 4, 5]

    def test_search_endpoint_accepts_match_axes(self, client, sample_pdf):
        doc_id = upload(client, sample_pdf).json()["data"]["document"]["id"]
        response = client.post(
            "/v1/search",
            json={
                "document": doc_id,
                "keywords": ["alpha"],
                "case_sensitive": False,
                "whole_words": False,
            },
        )
        assert response.status_code == 200
        data = response.json()["data"]
        assert data["case_sensitive"] is False
        assert data["whole_words"] is False

    def test_corpus_endpoint_recursive_and_rank(self, client, tmp_path):
        corpus = tmp_path / "corpus"
        (corpus / "sub").mkdir(parents=True)
        make_pdf(corpus / "a_one.pdf", ["alpha"])
        make_pdf(corpus / "sub" / "b_many.pdf", ["alpha alpha alpha"])
        response = client.post(
            "/v1/search/corpus",
            json={"keywords": ["alpha"], "directory": str(corpus), "recursive": True, "sort": "rank"},
        )
        assert response.status_code == 200
        data = response.json()["data"]
        assert data["documents_scanned"] == 2
        assert data["documents"][0]["document"]["filename"] == "b_many.pdf"
        assert data["documents"][0]["rank"] == 1

    def test_merge_folder_numeric_prefix_endpoint(self, client, tmp_path):
        folder = tmp_path / "pdfm"
        folder.mkdir()
        make_pdf(folder / "10_zeta.pdf", ["zeta"])
        make_pdf(folder / "2_beta.pdf", ["beta"])
        make_pdf(folder / "1_alpha.pdf", ["alpha"])
        response = client.post(
            "/v1/merge/folder",
            json={"directory": str(folder), "numeric_prefix": True},
        )
        assert response.status_code == 200
        data = response.json()["data"]
        assert [p.rsplit("/", 1)[-1] for p in data["sources"]] == [
            "1_alpha.pdf",
            "2_beta.pdf",
            "10_zeta.pdf",
        ]

    def test_speech_endpoint_with_voice_controls(self, tmp_path, speech_provider):
        app = create_app(
            Toolkit(strict=True, speech_provider=speech_provider), upload_dir=tmp_path / "up"
        )
        client = TestClient(app)
        response = client.post(
            "/v1/speech/synthesize",
            json={
                "text": "hello world",
                "save_path": str(tmp_path / "out.wav"),
                "rate": 200,
                "volume": 1.0,
            },
        )
        assert response.status_code == 200
        data = response.json()["data"]
        assert data["rate"] == 200.0
        assert data["volume"] == 1.0
        assert speech_provider.saved == [("hello world", str(tmp_path / "out.wav"), 200.0, 1.0)]
