"""The :class:`Toolkit` facade - the single public entry point of the engine.

Every facade method returns the uniform JSON envelope
(:mod:`pdftoolkit.core.result`). In the default non-strict mode failures
come back as ``{"ok": false, ...}`` envelopes; in strict mode
(``Toolkit(strict=True)``) the same typed errors propagate as exceptions,
which is what the HTTP adapter relies on.
"""

from __future__ import annotations

import os
from typing import Any, Callable, Dict, Optional, Sequence, Union

from .core.errors import PdfToolkitError
from .core.result import engine_info, failure, from_exception, success
from .documents.registry import DocumentRegistry
from .services import (
    LinksService,
    MergeService,
    PageOpsService,
    RenderService,
    SearchService,
    SpeechProvider,
    SpeechService,
    TextService,
    all_operations,
    catalog_as_dict,
)

PathLike = Union[str, "os.PathLike"]


class Toolkit:
    """Unified, headless PDF engine facade.

    Args:
        strict: when ``True``, operations raise typed errors instead of
            returning failure envelopes.
        speech_provider: optional speech backend (see
            :class:`~pdftoolkit.services.speech.SpeechProvider`).
        registry: optional pre-populated document registry.
    """

    def __init__(
        self,
        strict: bool = False,
        speech_provider: Optional[SpeechProvider] = None,
        registry: Optional[DocumentRegistry] = None,
    ) -> None:
        self._strict = bool(strict)
        self._speech_provider = speech_provider
        self._registry = registry if registry is not None else DocumentRegistry()
        self._services: Dict[str, Any] = {}

    # -- configuration -----------------------------------------------------

    @property
    def strict(self) -> bool:
        return self._strict

    @property
    def registry(self) -> DocumentRegistry:
        return self._registry

    def _service(self, name: str):
        """Build all services on first use, then serve them from cache."""
        if not self._services:
            from .services import build_services

            self._services = build_services(
                self._registry, speech_provider=self._speech_provider
            )
        return self._services[name]

    # -- lazy service accessors ---------------------------------------------

    @property
    def search_service(self) -> SearchService:
        return self._service("search")

    @property
    def text_service(self) -> TextService:
        return self._service("text")

    @property
    def links_service(self) -> LinksService:
        return self._service("links")

    @property
    def page_ops_service(self) -> PageOpsService:
        return self._service("page_ops")

    @property
    def merge_service(self) -> MergeService:
        return self._service("merge")

    @property
    def render_service(self) -> RenderService:
        return self._service("render")

    @property
    def speech_service(self) -> SpeechService:
        return self._service("speech")

    # -- envelope plumbing ---------------------------------------------------

    def _run(self, operation: str, action: Callable[[], Dict]) -> Dict:
        """Execute ``action`` and wrap the outcome in an envelope.

        Strict mode re-raises engine errors instead of catching them.
        """
        if self._strict:
            return success(operation, action())
        try:
            return success(operation, action())
        except PdfToolkitError as error:
            return failure(operation, error)
        except Exception as error:  # defensive: never leak raw tracebacks
            return from_exception(operation, error)

    # -- capabilities ---------------------------------------------------------

    def capabilities(self) -> Dict:
        """Describe the engine: identity, operations and service catalog."""
        return self._run(
            "capabilities",
            lambda: {
                "engine": engine_info(),
                "operations": all_operations(),
                "services": catalog_as_dict(),
            },
        )

    # -- document management ----------------------------------------------------

    def register(self, path: PathLike) -> Dict:
        """Register a document and receive its stable id and metadata."""
        return self._run("register", lambda: {"document": self._registry.register(path).to_dict()})

    def list_documents(self) -> Dict:
        """List every registered document."""
        return self._run(
            "list_documents",
            lambda: {"documents": [meta.to_dict() for meta in self._registry.list()]},
        )

    def document_info(self, reference: PathLike) -> Dict:
        """Return metadata for one document."""
        return self._run("document_info", lambda: {"document": self._registry.info(reference)})

    def remove_document(self, reference: PathLike) -> Dict:
        """Remove a document from the registry."""
        def _remove() -> Dict:
            meta = self._registry.remove(reference)
            return {"removed": meta.id, "filename": meta.filename}

        return self._run("remove_document", _remove)

    # -- search ------------------------------------------------------------------

    def search(
        self,
        reference: PathLike,
        keywords: Sequence[str],
        pages: Optional[str] = None,
        case_sensitive: bool = False,
        whole_words: bool = True,
    ) -> Dict:
        """Keyword-search one document.

        ``case_sensitive`` and ``whole_words`` toggle the legacy pdf-search
        matching behaviours; the defaults keep the hardened word-boundary,
        case-insensitive semantics.
        """
        return self._run(
            "search",
            lambda: self.search_service.search(
                reference, keywords, pages=pages, case_sensitive=case_sensitive, whole_words=whole_words
            ),
        )

    def search_corpus(
        self,
        keywords: Sequence[str],
        directory: Optional[PathLike] = None,
        pages: Optional[str] = None,
        case_sensitive: bool = False,
        whole_words: bool = True,
        recursive: bool = False,
        sort: str = "path",
    ) -> Dict:
        """Keyword-search every registered document, or a whole folder.

        ``recursive`` also scans sub-folders; ``sort="rank"`` orders the
        documents by total keyword occurrences (the original pdfgrep
        ranking behaviour).
        """
        return self._run(
            "search_corpus",
            lambda: self.search_service.search_corpus(
                keywords,
                directory=directory,
                pages=pages,
                case_sensitive=case_sensitive,
                whole_words=whole_words,
                recursive=recursive,
                sort=sort,
            ),
        )

    def context_pages(
        self,
        reference: PathLike,
        anchor: Union[int, str],
        before: int = 1,
        after: int = 1,
    ) -> Dict:
        """Return the pages around a keyword match or an explicit page."""
        return self._run(
            "context_pages",
            lambda: self.page_ops_service.context_pages(reference, anchor, before=before, after=after),
        )

    # -- extraction -----------------------------------------------------------------

    def extract_text(self, reference: PathLike, pages: Optional[str] = None) -> Dict:
        """Extract text from one document."""
        return self._run(
            "extract_text", lambda: self.text_service.extract_text(reference, pages=pages)
        )

    def extract_links(self, reference: PathLike, pages: Optional[str] = None) -> Dict:
        """Extract hyperlinks from one document."""
        return self._run(
            "extract_links", lambda: self.links_service.extract_links(reference, pages=pages)
        )

    def cut_pages(self, reference: PathLike, pages: str, output: PathLike) -> Dict:
        """Write a selection of pages into a new PDF."""
        return self._run(
            "cut_pages", lambda: self.page_ops_service.cut_pages(reference, pages, output)
        )

    def extract_context(
        self,
        reference: PathLike,
        anchor: Union[int, str],
        output: PathLike,
        before: int = 1,
        after: int = 1,
    ) -> Dict:
        """Extract the context window around an anchor into a new PDF."""
        return self._run(
            "extract_context",
            lambda: self.page_ops_service.extract_context(
                reference, anchor, output, before=before, after=after
            ),
        )

    def extract_matches(
        self,
        reference: PathLike,
        keywords: Sequence[str],
        output: PathLike,
        padding: int = 2,
        pages: Optional[str] = None,
        case_sensitive: bool = False,
        whole_words: bool = True,
    ) -> Dict:
        """Search a document and extract matches plus padding into a PDF.

        One-shot version of the original pdf-search workflow: matched
        pages are expanded by ``padding`` neighbours, overlapping windows
        merge and the union is written to ``output``.
        """
        return self._run(
            "extract_matches",
            lambda: self.page_ops_service.extract_matches(
                reference,
                keywords,
                output,
                padding=padding,
                pages=pages,
                case_sensitive=case_sensitive,
                whole_words=whole_words,
            ),
        )

    # -- merge ------------------------------------------------------------------------

    def merge(self, inputs: Sequence[PathLike], output: PathLike) -> Dict:
        """Merge at least two documents into one."""
        return self._run("merge", lambda: self.merge_service.merge(inputs, output))

    def merge_folder(
        self,
        directory: PathLike,
        output: Optional[PathLike] = None,
        recursive: bool = False,
        numeric_prefix: bool = False,
    ) -> Dict:
        """Merge every PDF in a folder (natural order, idempotent).

        ``numeric_prefix=True`` restricts the merge to ``N_name.pdf``
        files ordered by their leading number (the original pdfm tool).
        """
        return self._run(
            "merge_folder",
            lambda: self.merge_service.merge_folder(
                directory, output=output, recursive=recursive, numeric_prefix=numeric_prefix
            ),
        )

    # -- rendering -----------------------------------------------------------------------

    def render_pages(
        self,
        reference: PathLike,
        pages: Optional[str] = None,
        dpi: int = 150,
    ) -> Dict:
        """Render pages to base64 PNG payloads."""
        return self._run(
            "render_pages", lambda: self.render_service.render_pages(reference, pages=pages, dpi=dpi)
        )

    def render_to_files(
        self,
        reference: PathLike,
        output_dir: Optional[PathLike] = None,
        pages: Optional[str] = None,
        dpi: int = 150,
        prefix: Optional[str] = None,
    ) -> Dict:
        """Render pages to PNG files without overwriting existing files."""
        return self._run(
            "render_to_files",
            lambda: self.render_service.render_to_files(
                reference, output_dir=output_dir, pages=pages, dpi=dpi, prefix=prefix
            ),
        )

    def render_thumbnails(
        self,
        reference: PathLike,
        pages: Optional[str] = None,
        scale: float = 0.3,
    ) -> Dict:
        """Render lightweight page previews as base64 data URIs.

        Preserves the original pdf-search preview contract: a ``scale``
        factor (default ``0.3``) and ``data:image/png;base64,...`` payloads.
        """
        return self._run(
            "render_thumbnails",
            lambda: self.render_service.render_thumbnails(reference, pages=pages, scale=scale),
        )

    # -- speech ---------------------------------------------------------------------------

    def synthesize_speech(
        self,
        text: str,
        save_path: Optional[PathLike] = None,
        speak_aloud: bool = False,
        rate: Optional[float] = None,
        volume: Optional[float] = None,
    ) -> Dict:
        """Synthesise text to a file or aloud (exactly one mode).

        ``rate`` (words per minute) and ``volume`` (0.0 - 1.0) are optional
        voice controls forwarded to the speech provider.
        """
        return self._run(
            "synthesize_speech",
            lambda: self.speech_service.synthesize(
                text, save_path=save_path, speak_aloud=speak_aloud, rate=rate, volume=volume
            ),
        )
