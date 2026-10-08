"""Integration tests for the speech service and provider injection."""

import sys

import pytest

from helpers import FakeSpeechProvider
from pdftoolkit.core.errors import DependencyUnavailableError, ValidationError
from pdftoolkit.documents import DocumentRegistry
from pdftoolkit.services import SpeechService


@pytest.fixture
def service():
    return SpeechService(DocumentRegistry(), provider=FakeSpeechProvider())


class TestOutputModeValidation:
    def test_save_path_mode(self, service, tmp_path):
        target = tmp_path / "out.wav"
        result = service.synthesize("hello world", save_path=target)
        assert result["mode"] == "file"
        assert result["output"] == str(target)
        assert result["characters"] == len("hello world")
        assert service.provider.saved == [("hello world", str(target), None, None)]

    def test_speak_aloud_mode(self, service):
        result = service.synthesize("hello world", speak_aloud=True)
        assert result["mode"] == "aloud"
        assert result["spoken"] is True
        assert service.provider.spoken == [("hello world", None, None)]

    def test_both_modes_rejected(self, service, tmp_path):
        with pytest.raises(ValidationError) as excinfo:
            service.synthesize("text", save_path=tmp_path / "a.wav", speak_aloud=True)
        assert "exactly one output mode" in excinfo.value.message

    def test_neither_mode_rejected(self, service):
        with pytest.raises(ValidationError):
            service.synthesize("text")

    def test_empty_text_rejected(self, service):
        with pytest.raises(ValidationError):
            service.synthesize("", speak_aloud=True)

    def test_whitespace_text_rejected(self, service):
        with pytest.raises(ValidationError):
            service.synthesize("   ", speak_aloud=True)

    def test_none_text_rejected(self, service):
        with pytest.raises(ValidationError):
            service.synthesize(None, speak_aloud=True)

    def test_oversized_text_rejected(self, service):
        with pytest.raises(ValidationError):
            service.synthesize("x" * 1_000_001, speak_aloud=True)

    def test_save_creates_parent_directories(self, service, tmp_path):
        target = tmp_path / "deep" / "nested" / "audio.wav"
        service.synthesize("text", save_path=target)
        assert target.parent.is_dir()


class TestProviderInjection:
    def test_explicit_provider_used(self):
        provider = FakeSpeechProvider()
        service = SpeechService(DocumentRegistry(), provider=provider)
        service.synthesize("hi", speak_aloud=True)
        assert provider.spoken == [("hi", None, None)]

    def test_provider_failure_propagates(self, tmp_path):
        provider = FakeSpeechProvider(fail_on="save")
        service = SpeechService(DocumentRegistry(), provider=provider)
        with pytest.raises(RuntimeError):
            service.synthesize("hi", save_path=tmp_path / "x.wav")


class TestDefaultProvider:
    def test_missing_dependency_raises_503_code(self, monkeypatch):
        """When pyttsx3 is absent the engine reports dependency_unavailable."""
        monkeypatch.setitem(sys.modules, "pyttsx3", None)
        service = SpeechService(DocumentRegistry())
        with pytest.raises(DependencyUnavailableError) as excinfo:
            service.synthesize("hello", speak_aloud=True)
        assert excinfo.value.code == "dependency_unavailable"
        assert excinfo.value.http_status == 503

    def test_provider_is_cached(self):
        provider = FakeSpeechProvider()
        service = SpeechService(DocumentRegistry(), provider=provider)
        assert service.provider is provider
        assert service.provider is service.provider


class TestTextLengthBoundary:
    def test_exactly_at_limit_accepted(self):
        from pdftoolkit.services.speech import MAX_TEXT_LENGTH

        provider = FakeSpeechProvider()
        service = SpeechService(DocumentRegistry(), provider=provider)
        service.synthesize("x" * MAX_TEXT_LENGTH, speak_aloud=True)
        assert provider.spoken == [("x" * MAX_TEXT_LENGTH, None, None)]
