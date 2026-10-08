"""Unit tests for path validation and error primitives."""

import os
from pathlib import Path

import pytest

from pdftoolkit.core.errors import (
    ContentNotFoundError,
    DependencyUnavailableError,
    DocumentNotFoundError,
    OperationError,
    OutputConflictError,
    PageRangeError,
    PdfReadError,
    PdfToolkitError,
    ValidationError,
)
from pdftoolkit.core.validation import (
    require,
    require_non_empty_str,
    resolve_path,
)


class TestResolvePath:
    def test_existing_file_resolved_absolute(self, tmp_path):
        target = tmp_path / "doc.pdf"
        target.write_bytes(b"data")
        resolved = resolve_path(str(target))
        assert resolved == target.resolve()
        assert resolved.is_absolute()

    def test_pathlike_input_accepted(self, tmp_path):
        target = tmp_path / "doc.pdf"
        target.write_bytes(b"data")
        assert resolve_path(target) == target.resolve()

    def test_missing_file_rejected(self, tmp_path):
        with pytest.raises(ValidationError):
            resolve_path(tmp_path / "missing.pdf")

    def test_directory_rejected_as_file(self, tmp_path):
        (tmp_path / "adir").mkdir()
        with pytest.raises(ValidationError):
            resolve_path(tmp_path / "adir", kind="file")

    def test_directory_kind(self, tmp_path):
        (tmp_path / "adir").mkdir()
        assert resolve_path(tmp_path / "adir", kind="dir") == (tmp_path / "adir").resolve()

    def test_file_rejected_as_directory(self, tmp_path):
        target = tmp_path / "afile"
        target.write_bytes(b"x")
        with pytest.raises(ValidationError):
            resolve_path(target, kind="dir")

    def test_any_kind_accepts_file_and_dir(self, tmp_path):
        (tmp_path / "adir").mkdir()
        target = tmp_path / "afile"
        target.write_bytes(b"x")
        assert resolve_path(target, kind="any")
        assert resolve_path(tmp_path / "adir", kind="any")

    def test_must_exist_false_allows_missing(self, tmp_path):
        missing = tmp_path / "new" / "out.pdf"
        assert resolve_path(missing, must_exist=False) == missing.resolve()

    def test_none_rejected(self):
        with pytest.raises(ValidationError):
            resolve_path(None)

    def test_non_pathlike_rejected(self):
        with pytest.raises(ValidationError):
            resolve_path(42)

    def test_relative_path_resolved_from_cwd(self, tmp_path, monkeypatch):
        monkeypatch.chdir(tmp_path)
        (tmp_path / "doc.pdf").write_bytes(b"data")
        assert resolve_path("doc.pdf") == (tmp_path / "doc.pdf").resolve()

    def test_error_details_include_path(self, tmp_path):
        missing = tmp_path / "gone.pdf"
        with pytest.raises(ValidationError) as excinfo:
            resolve_path(missing)
        assert str(missing) in str(excinfo.value.details["path"])

    def test_invalid_kind_rejected(self, tmp_path):
        with pytest.raises(ValidationError):
            resolve_path(tmp_path, kind="bogus")


class TestRequireHelpers:
    def test_require_passes_on_true(self):
        require(True, "should not raise")

    def test_require_raises_on_false(self):
        with pytest.raises(ValidationError) as excinfo:
            require(False, "must hold", {"key": "value"})
        assert excinfo.value.message == "must hold"
        assert excinfo.value.details == {"key": "value"}

    def test_require_non_empty_str(self):
        assert require_non_empty_str("  hello  ", "name") == "hello"

    def test_require_non_empty_str_rejects_none(self):
        with pytest.raises(ValidationError):
            require_non_empty_str(None, "name")

    def test_require_non_empty_str_rejects_blank(self):
        with pytest.raises(ValidationError):
            require_non_empty_str("   ", "name")


class TestErrorHierarchy:
    @pytest.mark.parametrize(
        "error,code,status",
        [
            (ValidationError, "invalid_input", 400),
            (PageRangeError, "invalid_page_range", 400),
            (OutputConflictError, "output_conflict", 400),
            (PdfReadError, "unreadable_pdf", 400),
            (DocumentNotFoundError, "document_not_found", 404),
            (ContentNotFoundError, "content_not_found", 404),
            (DependencyUnavailableError, "dependency_unavailable", 503),
            (OperationError, "operation_failed", 500),
            (PdfToolkitError, "engine_error", 500),
        ],
    )
    def test_stable_codes_and_statuses(self, error, code, status):
        instance = error("boom")
        assert instance.code == code
        assert instance.http_status == status
        assert instance.message == "boom"

    def test_all_derive_from_base(self):
        for error in (
            ValidationError,
            PageRangeError,
            OutputConflictError,
            PdfReadError,
            DocumentNotFoundError,
            ContentNotFoundError,
            DependencyUnavailableError,
            OperationError,
        ):
            assert issubclass(error, PdfToolkitError)

    def test_validation_family(self):
        assert issubclass(PageRangeError, ValidationError)
        assert issubclass(OutputConflictError, ValidationError)

    def test_to_dict_shape(self):
        instance = ValidationError("bad input", {"field": "pages"})
        assert instance.to_dict() == {
            "code": "invalid_input",
            "message": "bad input",
            "details": {"field": "pages"},
        }

    def test_details_default_to_empty_dict(self):
        assert PdfToolkitError("x").details == {}

    def test_exceptions_are_catchable_as_group(self):
        with pytest.raises(PdfToolkitError):
            raise DocumentNotFoundError("gone")

    def test_os_pathlike_accepted_by_fspath_usage(self, tmp_path):
        # os.PathLike values flow through resolve_path unchanged semantically
        target = tmp_path / "ok.pdf"
        target.write_bytes(b"x")
        assert isinstance(resolve_path(target), Path)
        assert isinstance(os.fspath(target), str)
