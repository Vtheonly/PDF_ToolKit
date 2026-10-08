"""Adapter tests for the FastAPI HTTP surface.

Skipped automatically when the optional ``http`` extra (fastapi + httpx)
is not installed.
"""

import importlib.util
import json

import pytest

pytestmark = pytest.mark.skipif(
    importlib.util.find_spec("fastapi") is None
    or importlib.util.find_spec("httpx") is None,
    reason="fastapi/httpx extras not installed",
)

from fastapi.testclient import TestClient  # noqa: E402

from helpers import FakeSpeechProvider  # noqa: E402
from pdftoolkit.api.http import create_app  # noqa: E402
from pdftoolkit import Toolkit  # noqa: E402


@pytest.fixture
def client():
    engine = Toolkit(strict=True, speech_provider=FakeSpeechProvider())
    return TestClient(create_app(toolkit=engine))


def post(client, path, payload):
    return client.post(path, json=payload)


class TestHealthAndCapabilities:
    def test_health(self, client):
        response = client.get("/health")
        assert response.status_code == 200
        body = response.json()
        assert body["ok"] is True
        assert body["engine"]["name"] == "pdftoolkit"

    def test_capabilities_envelope(self, client):
        response = client.get("/v1/capabilities")
        assert response.status_code == 200
        envelope = response.json()
        assert set(envelope) == {"ok", "operation", "data", "error", "engine"}
        assert envelope["operation"] == "capabilities"
        assert "search" in envelope["data"]["operations"]


class TestDocumentEndpoints:
    def test_register_and_list(self, client, sample_pdf):
        response = post(client, "/v1/documents/register", {"path": str(sample_pdf)})
        assert response.status_code == 200
        assert response.json()["data"]["document"]["id"] == "doc-1"

        listing = client.get("/v1/documents")
        assert listing.status_code == 200
        assert len(listing.json()["data"]["documents"]) == 1

    def test_register_missing_field(self, client):
        response = post(client, "/v1/documents/register", {})
        assert response.status_code == 400
        assert response.json()["error"]["code"] == "invalid_input"

    def test_info_unknown_document(self, client):
        response = client.get("/v1/documents/doc-404")
        assert response.status_code == 404
        assert response.json()["error"]["code"] == "document_not_found"

    def test_remove_document(self, client, sample_pdf):
        post(client, "/v1/documents/register", {"path": str(sample_pdf)})
        response = client.delete("/v1/documents/doc-1")
        assert response.status_code == 200
        assert response.json()["data"]["removed"] == "doc-1"


class TestOperationEndpoints:
    def test_search(self, client, sample_pdf):
        response = post(
            client,
            "/v1/search",
            {"document": str(sample_pdf), "keywords": ["alpha", "beta"]},
        )
        assert response.status_code == 200
        data = response.json()["data"]
        assert data["totals"] == {"alpha": 3, "beta": 2}

    def test_search_with_page_filter(self, client, sample_pdf):
        response = post(
            client,
            "/v1/search",
            {"document": str(sample_pdf), "keywords": ["alpha"], "pages": "3"},
        )
        assert response.json()["data"]["totals"] == {"alpha": 2}

    def test_search_unknown_document_is_404(self, client, tmp_path):
        response = post(
            client,
            "/v1/search",
            {"document": str(tmp_path / "ghost.pdf"), "keywords": ["x"]},
        )
        assert response.status_code == 404

    def test_extract_text(self, client, sample_pdf):
        response = post(
            client, "/v1/extract/text", {"document": str(sample_pdf), "pages": "1"}
        )
        assert response.status_code == 200
        assert "alpha beta" in response.json()["data"]["pages"]["1"]

    def test_invalid_page_spec_is_400(self, client, sample_pdf):
        response = post(
            client, "/v1/extract/text", {"document": str(sample_pdf), "pages": "99"}
        )
        assert response.status_code == 400
        assert response.json()["error"]["code"] == "invalid_page_range"

    def test_extract_links(self, client, links_pdf):
        response = post(client, "/v1/extract/links", {"document": str(links_pdf)})
        data = response.json()["data"]
        assert data["count"] == 2

    def test_cut_pages(self, client, sample_pdf, tmp_path):
        response = post(
            client,
            "/v1/pages/cut",
            {
                "document": str(sample_pdf),
                "pages": "1,3",
                "output": str(tmp_path / "cut.pdf"),
            },
        )
        assert response.status_code == 200
        assert response.json()["data"]["pages"] == 2

    def test_cut_output_conflict_is_400(self, client, sample_pdf):
        response = post(
            client,
            "/v1/pages/cut",
            {"document": str(sample_pdf), "pages": "1", "output": str(sample_pdf)},
        )
        assert response.status_code == 400
        assert response.json()["error"]["code"] == "output_conflict"

    def test_context_pages(self, client, sample_pdf):
        response = post(
            client,
            "/v1/pages/context",
            {"document": str(sample_pdf), "anchor": "gamma"},
        )
        data = response.json()["data"]
        assert data["anchor_page"] == 2
        assert data["pages"] == [1, 2, 3]

    def test_context_missing_keyword_is_404(self, client, sample_pdf):
        response = post(
            client,
            "/v1/pages/context",
            {"document": str(sample_pdf), "anchor": "nope"},
        )
        assert response.status_code == 404
        assert response.json()["error"]["code"] == "content_not_found"

    def test_extract_context(self, client, sample_pdf, tmp_path):
        response = post(
            client,
            "/v1/pages/extract-context",
            {
                "document": str(sample_pdf),
                "anchor": "gamma",
                "output": str(tmp_path / "ctx.pdf"),
            },
        )
        assert response.status_code == 200
        assert response.json()["data"]["pages"] == 3

    def test_merge(self, client, multi_pdf, tmp_path):
        response = post(
            client,
            "/v1/merge",
            {"inputs": [str(p) for p in multi_pdf], "output": str(tmp_path / "m.pdf")},
        )
        assert response.status_code == 200
        assert response.json()["data"]["pages"] == 3

    def test_merge_single_input_is_400(self, client, sample_pdf, tmp_path):
        response = post(
            client,
            "/v1/merge",
            {"inputs": [str(sample_pdf)], "output": str(tmp_path / "m.pdf")},
        )
        assert response.status_code == 400
        assert response.json()["error"]["code"] == "invalid_input"

    def test_merge_folder(self, client, merge_dir):
        response = post(
            client, "/v1/merge/folder", {"directory": str(merge_dir)}
        )
        assert response.status_code == 200
        data = response.json()["data"]
        assert data["files"] == 3
        assert data["pages"] == 6

    def test_render_pages(self, client, sample_pdf):
        response = post(
            client,
            "/v1/render/pages",
            {"document": str(sample_pdf), "pages": "1", "dpi": 72},
        )
        assert response.status_code == 200
        assert set(response.json()["data"]["images"]) == {"1"}


class TestSpeechEndpoint:
    def test_synthesize_to_file(self, client, tmp_path):
        response = post(
            client,
            "/v1/speech/synthesize",
            {"text": "hello", "save_path": str(tmp_path / "a.wav")},
        )
        assert response.status_code == 200
        assert response.json()["data"]["mode"] == "file"

    def test_both_modes_is_400(self, client, tmp_path):
        response = post(
            client,
            "/v1/speech/synthesize",
            {
                "text": "hello",
                "save_path": str(tmp_path / "a.wav"),
                "speak_aloud": True,
            },
        )
        assert response.status_code == 400
        assert response.json()["error"]["code"] == "invalid_input"

    def test_missing_text_is_400(self, client):
        response = post(client, "/v1/speech/synthesize", {"speak_aloud": True})
        assert response.status_code == 400

    def test_dependency_unavailable_is_503(self, monkeypatch):
        import sys

        monkeypatch.setitem(sys.modules, "pyttsx3", None)
        engine = Toolkit(strict=True)  # no injected provider
        local_client = TestClient(create_app(toolkit=engine))
        response = post(
            local_client, "/v1/speech/synthesize", {"text": "hi", "speak_aloud": True}
        )
        assert response.status_code == 503
        assert response.json()["error"]["code"] == "dependency_unavailable"


class TestHttpEnvelopeContract:
    def test_every_response_is_an_envelope(self, client, sample_pdf):
        for response in (
            client.get("/v1/capabilities"),
            post(client, "/v1/extract/text", {"document": str(sample_pdf)}),
        ):
            assert set(response.json()) == {"ok", "operation", "data", "error", "engine"}

    def test_error_envelopes_are_json_serialisable(self, client):
        response = client.get("/v1/documents/ghost")
        assert json.loads(json.dumps(response.json())) == response.json()

    def test_status_mapping_matrix(self, client, sample_pdf, tmp_path):
        """400 / 404 / 400 paths across operations."""
        cases = [
            (post(client, "/v1/search", {"document": str(sample_pdf)}), 400),
            (post(client, "/v1/search", {"document": "ghost-doc", "keywords": ["x"]}), 404),
            (
                post(
                    client,
                    "/v1/merge",
                    {"inputs": [str(sample_pdf)], "output": str(tmp_path / "o.pdf")},
                ),
                400,
            ),
        ]
        for response, expected_status in cases:
            assert response.status_code == expected_status
