"""Integration tests for speech voice controls (rate and volume, mirroring
the original textToSpeach utility's pyttsx3 properties)."""

import pytest

from helpers import FakeSpeechProvider
from pdftoolkit.core.errors import ValidationError
from pdftoolkit.documents import DocumentRegistry
from pdftoolkit.services import SpeechService


@pytest.fixture
def provider():
    return FakeSpeechProvider()


@pytest.fixture
def service(provider):
    return SpeechService(DocumentRegistry(), provider=provider)


class TestRateControl:
    def test_rate_forwarded_to_provider_on_save(self, service, provider, tmp_path):
        service.synthesize("hello", save_path=tmp_path / "a.wav", rate=200)
        assert provider.saved == [("hello", str(tmp_path / "a.wav"), 200.0, None)]

    def test_rate_forwarded_to_provider_on_speak(self, service, provider):
        service.synthesize("hello", speak_aloud=True, rate=180)
        assert provider.spoken == [("hello", 180.0, None)]

    def test_result_reports_rate(self, service, tmp_path):
        result = service.synthesize("hello", speak_aloud=True, rate=200)
        assert result["rate"] == 200.0

    def test_float_rate_accepted(self, service, provider):
        service.synthesize("hello", speak_aloud=True, rate=175.5)
        assert provider.spoken == [("hello", 175.5, None)]

    @pytest.mark.parametrize("rate", [0, -10, 5000, "fast", True])
    def test_invalid_rate_rejected(self, service, rate):
        with pytest.raises(ValidationError):
            service.synthesize("hello", speak_aloud=True, rate=rate)

    def test_boundary_rates_accepted(self, service):
        assert service.synthesize("hi", speak_aloud=True, rate=1)["rate"] == 1.0
        assert service.synthesize("hi", speak_aloud=True, rate=1000)["rate"] == 1000.0


class TestVolumeControl:
    def test_volume_forwarded_to_provider_on_save(self, service, provider, tmp_path):
        service.synthesize("hello", save_path=tmp_path / "a.wav", volume=0.7)
        assert provider.saved == [("hello", str(tmp_path / "a.wav"), None, 0.7)]

    def test_volume_forwarded_to_provider_on_speak(self, service, provider):
        service.synthesize("hello", speak_aloud=True, volume=1.0)
        assert provider.spoken == [("hello", None, 1.0)]

    def test_result_reports_volume(self, service):
        result = service.synthesize("hello", speak_aloud=True, volume=0.5)
        assert result["volume"] == 0.5

    @pytest.mark.parametrize("volume", [-0.1, 1.5, "loud", True])
    def test_invalid_volume_rejected(self, service, volume):
        with pytest.raises(ValidationError):
            service.synthesize("hello", speak_aloud=True, volume=volume)

    def test_boundary_volumes_accepted(self, service):
        assert service.synthesize("hi", speak_aloud=True, volume=0.0)["volume"] == 0.0
        assert service.synthesize("hi", speak_aloud=True, volume=1.0)["volume"] == 1.0


class TestCombinedVoiceControls:
    def test_rate_and_volume_together(self, service, provider, tmp_path):
        # The original utility used rate=200 and volume=1.0 together.
        service.synthesize(
            "Hello, how can I assist you today?",
            save_path=tmp_path / "speech.wav",
            rate=200,
            volume=1.0,
        )
        assert provider.saved == [
            ("Hello, how can I assist you today?", str(tmp_path / "speech.wav"), 200.0, 1.0)
        ]

    def test_omitted_controls_default_to_none(self, service, provider, tmp_path):
        service.synthesize("plain", save_path=tmp_path / "plain.wav")
        assert provider.saved == [("plain", str(tmp_path / "plain.wav"), None, None)]

    def test_mode_validation_still_applies_with_controls(self, service, tmp_path):
        with pytest.raises(ValidationError):
            service.synthesize(
                "text", save_path=tmp_path / "a.wav", speak_aloud=True, rate=200
            )

    def test_controls_do_not_bypass_text_validation(self, service):
        with pytest.raises(ValidationError):
            service.synthesize("", speak_aloud=True, rate=200)
