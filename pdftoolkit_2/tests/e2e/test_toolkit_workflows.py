"""End-to-end tests: real workflows through the Toolkit facade.

These exercise the public contract exactly as an external consumer would:
JSON envelopes in, structured data out - across document management, search,
extraction, page operations, merging, rendering and speech.
"""

import base64
import json

import pytest

from pdftoolkit import Toolkit
from pdftoolkit.core.errors import DocumentNotFoundError, PageRangeError, ValidationError

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
ENVELOPE_KEYS = {"ok", "operation", "data", "error", "engine"}


def assert_success(envelope, operation):
    assert set(envelope) == ENVELOPE_KEYS
    assert envelope["ok"] is True
    assert envelope["operation"] == operation
    assert envelope["error"] is None
    assert envelope["engine"]["name"] == "pdftoolkit"
    assert isinstance(envelope["data"], dict)


def assert_failure(envelope, operation, code):
    assert set(envelope) == ENVELOPE_KEYS
    assert envelope["ok"] is False
    assert envelope["operation"] == operation
    assert envelope["data"] is None
    assert envelope["error"]["code"] == code


class TestEnvelopeContract:
    """Every facade method must obey the one envelope contract."""

    def test_capabilities(self, engine):
        envelope = engine.capabilities()
        assert_success(envelope, "capabilities")
        operations = envelope["data"]["operations"]
        for expected in (
            "search",
            "search_corpus",
            "extract_text",
            "extract_links",
            "cut_pages",
            "context_pages",
            "extract_context",
            "merge",
            "merge_folder",
            "render_pages",
            "render_to_files",
            "synthesize_speech",
        ):
            assert expected in operations

    def test_register(self, engine, sample_pdf):
        assert_success(engine.register(sample_pdf), "register")

    def test_list_and_info_and_remove(self, engine, sample_pdf):
        engine.register(sample_pdf)
        assert_success(engine.list_documents(), "list_documents")
        assert_success(engine.document_info("doc-1"), "document_info")
        assert_success(engine.remove_document("doc-1"), "remove_document")

    def test_search_envelope(self, engine, sample_pdf):
        assert_success(engine.search(sample_pdf, ["alpha"]), "search")

    def test_search_corpus_envelope(self, engine, corpus_dir):
        assert_success(
            engine.search_corpus(["alpha"], directory=corpus_dir), "search_corpus"
        )

    def test_extract_text_envelope(self, engine, sample_pdf):
        assert_success(engine.extract_text(sample_pdf), "extract_text")

    def test_extract_links_envelope(self, engine, links_pdf):
        assert_success(engine.extract_links(links_pdf), "extract_links")

    def test_cut_pages_envelope(self, engine, sample_pdf, tmp_path):
        assert_success(
            engine.cut_pages(sample_pdf, "1-2", tmp_path / "cut.pdf"), "cut_pages"
        )

    def test_context_pages_envelope(self, engine, sample_pdf):
        assert_success(engine.context_pages(sample_pdf, "gamma"), "context_pages")

    def test_extract_context_envelope(self, engine, sample_pdf, tmp_path):
        assert_success(
            engine.extract_context(sample_pdf, "gamma", tmp_path / "ctx.pdf"),
            "extract_context",
        )

    def test_merge_envelope(self, engine, multi_pdf, tmp_path):
        assert_success(engine.merge(multi_pdf, tmp_path / "merged.pdf"), "merge")

    def test_merge_folder_envelope(self, engine, merge_dir):
        assert_success(engine.merge_folder(merge_dir), "merge_folder")

    def test_render_envelope(self, engine, sample_pdf):
        assert_success(engine.render_pages(sample_pdf, pages="1"), "render_pages")

    def test_render_files_envelope(self, engine, sample_pdf, tmp_path):
        assert_success(
            engine.render_to_files(sample_pdf, output_dir=tmp_path / "r"),
            "render_to_files",
        )

    def test_speech_envelope(self, engine, tmp_path):
        assert_success(
            engine.synthesize_speech("hello", save_path=tmp_path / "a.wav"),
            "synthesize_speech",
        )

    def test_every_envelope_is_json_serialisable(self, engine, sample_pdf, tmp_path):
        envelopes = [
            engine.capabilities(),
            engine.register(sample_pdf),
            engine.search(sample_pdf, ["alpha"]),
            engine.extract_text(sample_pdf, pages="1"),
            engine.cut_pages(sample_pdf, "1", tmp_path / "c.pdf"),
            engine.document_info(sample_pdf),
        ]
        for envelope in envelopes:
            assert json.loads(json.dumps(envelope)) == envelope


class TestFailureEnvelopes:
    def test_unknown_document(self, engine, tmp_path):
        envelope = engine.search(tmp_path / "ghost.pdf", ["alpha"])
        assert_failure(envelope, "search", "document_not_found")

    def test_invalid_page_spec(self, engine, sample_pdf):
        envelope = engine.extract_text(sample_pdf, pages="99")
        assert_failure(envelope, "extract_text", "invalid_page_range")

    def test_output_conflict(self, engine, sample_pdf):
        envelope = engine.cut_pages(sample_pdf, "1", sample_pdf)
        assert_failure(envelope, "cut_pages", "output_conflict")

    def test_unreadable_pdf(self, engine, corrupt_pdf):
        envelope = engine.extract_text(corrupt_pdf)
        assert_failure(envelope, "extract_text", "unreadable_pdf")

    def test_merge_needs_two_files(self, engine, sample_pdf, tmp_path):
        envelope = engine.merge([sample_pdf], tmp_path / "out.pdf")
        assert_failure(envelope, "merge", "invalid_input")

    def test_speech_needs_exactly_one_mode(self, engine, tmp_path):
        envelope = engine.synthesize_speech("text")
        assert_failure(envelope, "synthesize_speech", "invalid_input")

    def test_failure_envelopes_are_json_serialisable(self, engine, tmp_path):
        envelope = engine.search(tmp_path / "ghost.pdf", ["x"])
        assert json.loads(json.dumps(envelope)) == envelope

    def test_engine_state_survives_failures(self, engine, sample_pdf):
        engine.register(sample_pdf)
        engine.extract_text("doc-404")
        assert engine.list_documents()["data"]["documents"][0]["id"] == "doc-1"


class TestStrictMode:
    def test_strict_success_still_returns_envelope(self, strict_engine, sample_pdf):
        envelope = strict_engine.search(sample_pdf, ["alpha"])
        assert_success(envelope, "search")

    def test_strict_failure_raises(self, strict_engine, tmp_path):
        with pytest.raises(DocumentNotFoundError):
            strict_engine.search(tmp_path / "ghost.pdf", ["alpha"])

    def test_strict_page_error_raises(self, strict_engine, sample_pdf):
        with pytest.raises(PageRangeError):
            strict_engine.extract_text(sample_pdf, pages="99")

    def test_strict_validation_error_raises(self, strict_engine, tmp_path):
        with pytest.raises(ValidationError):
            strict_engine.synthesize_speech("text")


class TestFullWorkflow:
    def test_document_lifecycle(self, engine, sample_pdf):
        registered = engine.register(sample_pdf)
        document_id = registered["data"]["document"]["id"]
        assert document_id == "doc-1"

        info = engine.document_info(document_id)
        assert info["data"]["document"]["page_count"] == 5

        listing = engine.list_documents()
        assert len(listing["data"]["documents"]) == 1

        removed = engine.remove_document(document_id)
        assert removed["data"]["removed"] == document_id
        assert engine.list_documents()["data"]["documents"] == []

    def test_search_to_context_to_extraction(self, engine, sample_pdf, tmp_path):
        search = engine.search(sample_pdf, ["gamma"])
        assert search["data"]["totals"] == {"gamma": 2}

        context = engine.context_pages(sample_pdf, "gamma")
        assert context["data"]["anchor_page"] == 2

        extracted = engine.extract_context(sample_pdf, "gamma", tmp_path / "ctx.pdf")
        assert extracted["data"]["pages"] == 3

        text = engine.extract_text(tmp_path / "ctx.pdf")
        assert text["data"]["page_count"] == 3

    def test_merge_then_verify(self, engine, merge_dir, tmp_path):
        merged = engine.merge_folder(merge_dir, output=tmp_path / "combo.pdf")
        assert merged["data"]["pages"] == 6

        verification = engine.search(tmp_path / "combo.pdf", ["zeta"])
        assert verification["data"]["totals"] == {"zeta": 3}

    def test_render_roundtrip(self, engine, sample_pdf):
        rendered = engine.render_pages(sample_pdf, pages="2", dpi=72)
        raw = base64.b64decode(rendered["data"]["images"]["2"])
        assert raw.startswith(PNG_MAGIC)

    def test_speech_workflow(self, engine, tmp_path, speech_provider):
        result = engine.synthesize_speech("invoice ready", save_path=tmp_path / "speech.wav")
        assert result["data"]["output"] == str(tmp_path / "speech.wav")
        assert speech_provider.saved == [("invoice ready", str(tmp_path / "speech.wav"))]

    def test_corpus_workflow(self, engine, corpus_dir):
        result = engine.search_corpus(["alpha"], directory=corpus_dir)
        assert result["data"]["totals"] == {"alpha": 2}
        assert result["data"]["skipped"][0]["reason"] == "unreadable_pdf"


class TestFacadePlumbing:
    def test_strict_flag_exposed(self, engine, strict_engine):
        assert engine.strict is False
        assert strict_engine.strict is True

    def test_registry_shared_across_services(self, engine, sample_pdf):
        engine.register(sample_pdf)
        # text and links services must see the same registry
        assert engine.text_service.registry is engine.links_service.registry

    def test_services_are_cached(self, engine):
        assert engine.search_service is engine.search_service

    def test_default_construction(self, sample_pdf):
        fresh = Toolkit()
        envelope = fresh.search(sample_pdf, ["alpha"])
        assert envelope["ok"] is True
