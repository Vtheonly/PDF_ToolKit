"""Speech synthesis service with an injectable provider backend.

The engine stays free of hard speech dependencies: the default provider
loads ``pyttsx3`` lazily and reports :class:`DependencyUnavailableError`
when the optional ``pdftoolkit[speech]`` extra is not installed. Tests and
alternative runtimes inject their own provider.
"""

from __future__ import annotations

from typing import Dict, Optional, Protocol

from ..core.errors import DependencyUnavailableError, ValidationError
from ..core.validation import require_non_empty_str, resolve_path
from ..utils.files import ensure_parent
from .base import Service

MAX_TEXT_LENGTH = 1_000_000


class SpeechProvider(Protocol):
    """Minimal provider contract: persist audio or speak it aloud."""

    def save(self, text: str, path: str) -> None: ...

    def speak(self, text: str) -> None: ...


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

    def save(self, text: str, path: str) -> None:
        engine = self._pyttsx3.init()
        try:
            engine.save_to_file(text, path)
            engine.runAndWait()
        finally:
            try:
                engine.stop()
            except Exception:  # pragma: no cover - engine may already be down
                pass

    def speak(self, text: str) -> None:
        engine = self._pyttsx3.init()
        try:
            engine.say(text)
            engine.runAndWait()
        finally:
            try:
                engine.stop()
            except Exception:  # pragma: no cover
                pass


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
    ) -> Dict:
        """Synthesise ``text`` either to a file or aloud - never both.

        Exactly one of ``save_path`` / ``speak_aloud`` must be requested;
        passing both or neither is rejected.
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

        if save_path:
            target = resolve_path(save_path, must_exist=False, kind="any")
            ensure_parent(target)
            self.provider.save(content, str(target))
            return {
                "output": str(target),
                "characters": len(content),
                "mode": "file",
            }

        self.provider.speak(content)
        return {"spoken": True, "characters": len(content), "mode": "aloud"}
