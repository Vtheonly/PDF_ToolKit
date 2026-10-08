"""Speech synthesis service with an injectable provider backend.

The engine stays free of hard speech dependencies: the default provider
loads ``pyttsx3`` lazily and reports :class:`DependencyUnavailableError`
when the optional ``pdftoolkit[speech]`` extra is not installed. Tests and
alternative runtimes inject their own provider.

``rate`` (words per minute) and ``volume`` (0.0 - 1.0) mirror the voice
controls of the original ``textToSpeach`` utility and are applied by the
provider when given.
"""

from __future__ import annotations

from typing import Dict, Optional, Protocol

from ..core.errors import DependencyUnavailableError, ValidationError
from ..core.validation import require_non_empty_str, resolve_path
from ..utils.files import ensure_parent
from .base import Service

MAX_TEXT_LENGTH = 1_000_000
MIN_RATE = 1
MAX_RATE = 1000
MIN_VOLUME = 0.0
MAX_VOLUME = 1.0


class SpeechProvider(Protocol):
    """Minimal provider contract: persist audio or speak it aloud."""

    def save(self, text: str, path: str, rate: Optional[float] = None, volume: Optional[float] = None) -> None: ...

    def speak(self, text: str, rate: Optional[float] = None, volume: Optional[float] = None) -> None: ...


class PyttsxProvider:
    """Default provider backed by the optional ``pyttsx3`` dependency."""

    def __init__(self) -> None:
        try:
            import pyttsx3  # noqa: WPS433 (runtime optional dependency)
        except ImportError as exc:  # pragma: no cover - depends on host env
            raise DependencyUnavailableError(
                "speech backend unavailable; install the optional extra: "
                "pip install 'pdftoolkit[speech]'"
            ) from exc
        self._pyttsx3 = pyttsx3

    def save(
        self,
        text: str,
        path: str,
        rate: Optional[float] = None,
        volume: Optional[float] = None,
    ) -> None:
        engine = self._pyttsx3.init()
        try:
            _apply_voice_properties(engine, rate, volume)
            engine.save_to_file(text, path)
            engine.runAndWait()
        finally:
            try:
                engine.stop()
            except Exception:  # pragma: no cover - engine may already be down
                pass

    def speak(
        self,
        text: str,
        rate: Optional[float] = None,
        volume: Optional[float] = None,
    ) -> None:
        engine = self._pyttsx3.init()
        try:
            _apply_voice_properties(engine, rate, volume)
            engine.say(text)
            engine.runAndWait()
        finally:
            try:
                engine.stop()
            except Exception:  # pragma: no cover
                pass


def _apply_voice_properties(engine, rate: Optional[float], volume: Optional[float]) -> None:
    """Set ``rate``/``volume`` on a pyttsx3 engine when provided."""
    if rate is not None:
        engine.setProperty("rate", int(rate))
    if volume is not None:
        engine.setProperty("volume", float(volume))


class SpeechService(Service):
    """Convert text to speech with exactly one output mode per call."""

    def __init__(self, registry, provider: Optional[SpeechProvider] = None) -> None:
        super().__init__(registry)
        self._provider = provider

    @property
    def provider(self) -> SpeechProvider:
        """Return the injected provider or lazily create the default one."""
        if self._provider is None:
            self._provider = PyttsxProvider()
        return self._provider

    def synthesize(
        self,
        text: str,
        save_path: Optional = None,
        speak_aloud: bool = False,
        rate: Optional[float] = None,
        volume: Optional[float] = None,
    ) -> Dict:
        """Synthesise ``text`` either to a file or aloud - never both.

        Exactly one of ``save_path`` / ``speak_aloud`` must be requested;
        passing both or neither is rejected. ``rate`` (words per minute)
        and ``volume`` (0.0 - 1.0) are optional voice controls forwarded to
        the provider.
        """
        content = require_non_empty_str(text, "text")
        if len(content) > MAX_TEXT_LENGTH:
            raise ValidationError(
                f"text exceeds the maximum length of {MAX_TEXT_LENGTH} characters",
                {"length": len(content), "max": MAX_TEXT_LENGTH},
            )

        if bool(save_path) == bool(speak_aloud):
            raise ValidationError(
                "exactly one output mode is required: provide either "
                "save_path or speak_aloud, but not both",
                {"save_path": save_path is not None, "speak_aloud": bool(speak_aloud)},
            )

        rate = self._validate_rate(rate)
        volume = self._validate_volume(volume)

        if save_path:
            target = resolve_path(save_path, must_exist=False, kind="any")
            ensure_parent(target)
            self.provider.save(content, str(target), rate=rate, volume=volume)
            return {
                "output": str(target),
                "characters": len(content),
                "mode": "file",
                "rate": rate,
                "volume": volume,
            }

        self.provider.speak(content, rate=rate, volume=volume)
        return {
            "spoken": True,
            "characters": len(content),
            "mode": "aloud",
            "rate": rate,
            "volume": volume,
        }

    # -- internals ----------------------------------------------------------

    @staticmethod
    def _validate_rate(rate) -> Optional[float]:
        if rate is None:
            return None
        if isinstance(rate, bool) or not isinstance(rate, (int, float)):
            raise ValidationError(f"rate must be a number, got {rate!r}")
        rate = float(rate)
        if rate < MIN_RATE or rate > MAX_RATE:
            raise ValidationError(
                f"rate must be between {MIN_RATE} and {MAX_RATE} words per minute, got {rate}",
                {"rate": rate, "min": MIN_RATE, "max": MAX_RATE},
            )
        return rate

    @staticmethod
    def _validate_volume(volume) -> Optional[float]:
        if volume is None:
            return None
        if isinstance(volume, bool) or not isinstance(volume, (int, float)):
            raise ValidationError(f"volume must be a number, got {volume!r}")
        volume = float(volume)
        if volume < MIN_VOLUME or volume > MAX_VOLUME:
            raise ValidationError(
                f"volume must be between {MIN_VOLUME} and {MAX_VOLUME}, got {volume}",
                {"volume": volume, "min": MIN_VOLUME, "max": MAX_VOLUME},
            )
        return volume
