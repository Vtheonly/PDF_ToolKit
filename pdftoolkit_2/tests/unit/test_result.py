"""Unit tests for the uniform result envelope."""

import pytest

from pdftoolkit.core.errors import DocumentNotFoundError, ValidationError
from pdftoolkit.core.result import (
    ENGINE_NAME,
    ENGINE_VERSION,
    engine_info,
    failure,
    from_exception,
    success,
)


class TestEngineInfo:
    def test_identity(self):
        assert ENGINE_NAME == "pdftoolkit"
        assert ENGINE_VERSION == "1.0.0"

    def test_engine_info_block(self):
        assert engine_info() == {"name": "pdftoolkit", "version": "1.0.0"}


class TestSuccessEnvelope:
    def test_shape(self):
        envelope = success("search", {"totals": {}})
        assert envelope == {
            "ok": True,
            "operation": "search",
            "data": {"totals": {}},
            "error": None,
            "engine": {"name": "pdftoolkit", "version": "1.0.0"},
        }

    def test_data_defaults_to_empty_dict(self):
        assert success("op")["data"] == {}

    def test_operation_passthrough(self):
        assert success("merge_folder")["operation"] == "merge_folder"


class TestFailureEnvelope:
    def test_shape_from_typed_error(self):
        error = DocumentNotFoundError("gone", {"id": "doc-9"})
        envelope = failure("document_info", error)
        assert envelope["ok"] is False
        assert envelope["operation"] == "document_info"
        assert envelope["data"] is None
        assert envelope["error"]["code"] == "document_not_found"
        assert envelope["error"]["message"] == "gone"
        assert envelope["error"]["details"] == {"id": "doc-9"}
        assert envelope["engine"]["version"] == ENGINE_VERSION


class TestFromException:
    def test_typed_errors_pass_through(self):
        error = ValidationError("nope")
        envelope = from_exception("op", error)
        assert envelope["error"]["code"] == "invalid_input"
        assert envelope["error"]["message"] == "nope"

    def test_unknown_exceptions_are_wrapped(self):
        envelope = from_exception("op", RuntimeError("kaboom"))
        assert envelope["ok"] is False
        assert envelope["error"]["code"] == "operation_failed"
        assert "kaboom" in envelope["error"]["message"]


class TestEnvelopeContract:
    """The five keys and their types are the public API contract."""

    @pytest.mark.parametrize("envelope", [
        success("op"),
        failure("op", ValidationError("x")),
        from_exception("op", ValueError("y")),
    ])
    def test_keys_are_stable(self, envelope):
        assert set(envelope) == {"ok", "operation", "data", "error", "engine"}

    def test_ok_and_error_are_mutually_exclusive(self):
        assert success("op")["error"] is None
        assert failure("op", ValidationError("x"))["data"] is None
