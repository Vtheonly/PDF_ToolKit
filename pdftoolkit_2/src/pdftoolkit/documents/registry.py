"""Thread-safe registry of documents known to the engine.

Documents are addressed by stable ids (``doc-1``, ``doc-2``, ...). Any API
also accepts a path; unknown-but-existing paths are auto-registered so the
engine is convenient to drive from stateless callers (CLI/HTTP) while
remaining session-friendly for programmatic use.
"""

from __future__ import annotations

import os
import threading
from typing import Dict, List, Union

from ..core.errors import DocumentNotFoundError, ValidationError
from ..core.models import DocumentMeta
from ..core.validation import resolve_path
from ..io.pdfio import open_pdf
from ..utils.naming import natural_key

PathLike = Union[str, "os.PathLike"]


class DocumentRegistry:
    """Thread-safe store of :class:`DocumentMeta` entries.

    Registration is atomic: a document only becomes visible once its page
    count has been read successfully, so the registry never holds a
    half-initialised or unreadable entry.
    """

    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._documents: Dict[str, DocumentMeta] = {}
        self._by_path: Dict[str, str] = {}
        self._counter = 0

    # -- registration ------------------------------------------------------

    def register(self, path: PathLike) -> DocumentMeta:
        """Register a document and return its metadata.

        Re-registering an existing path returns the existing entry unchanged.
        Unreadable PDFs raise :class:`~pdftoolkit.core.errors.PdfReadError`
        without modifying the registry.
        """
        resolved = resolve_path(path, must_exist=True, kind="file")
        key = str(resolved).lower()

        with self._lock:
            existing_id = self._by_path.get(key)
            if existing_id is not None:
                return self._documents[existing_id]

        # Opening the PDF is the expensive part; do it outside the lock so
        # concurrent registrations do not serialise on file IO.
        with open_pdf(resolved) as doc:
            page_count = doc.page_count

        with self._lock:
            # Re-check: another thread may have registered meanwhile.
            existing_id = self._by_path.get(key)
            if existing_id is not None:
                return self._documents[existing_id]

            self._counter += 1
            meta = DocumentMeta(
                id=f"doc-{self._counter}",
                path=str(resolved),
                filename=resolved.name,
                page_count=page_count,
            )
            self._documents[meta.id] = meta
            self._by_path[key] = meta.id
            return meta

    # -- lookup ------------------------------------------------------------

    def resolve(self, reference: PathLike) -> DocumentMeta:
        """Resolve an id or a path to a registered :class:`DocumentMeta`.

        Paths that exist on disk but are not registered are auto-registered.
        Unknown ids or missing paths raise :class:`DocumentNotFoundError`.
        """
        if reference is None:
            raise ValidationError("a document reference is required")

        with self._lock:
            if isinstance(reference, str) and reference in self._documents:
                return self._documents[reference]

        if not isinstance(reference, (str, os.PathLike)):
            raise ValidationError(f"invalid document reference: {reference!r}")

        resolved = resolve_path(reference, must_exist=False, kind="file")
        if resolved.exists():
            return self.register(resolved)

        raise DocumentNotFoundError(
            f"document not found: {reference}",
            {"reference": str(reference)},
        )

    def get(self, document_id: str) -> DocumentMeta:
        """Return metadata by exact id."""
        with self._lock:
            meta = self._documents.get(document_id)
        if meta is None:
            raise DocumentNotFoundError(
                f"document not found: {document_id}", {"id": document_id}
            )
        return meta

    # -- queries -----------------------------------------------------------

    def list(self) -> List[DocumentMeta]:
        """Return all registered documents ordered by id."""
        with self._lock:
            metas = list(self._documents.values())
        return sorted(metas, key=lambda meta: natural_key(meta.id))

    def page_count(self, reference: PathLike) -> int:
        """Return the (cached) page count of a document."""
        return self.resolve(reference).page_count or 0

    def info(self, reference: PathLike) -> Dict:
        """Return a JSON-safe summary of one document."""
        return self.resolve(reference).to_dict()

    # -- lifecycle ---------------------------------------------------------

    def remove(self, reference: PathLike) -> DocumentMeta:
        """Remove a document from the registry and return its metadata."""
        meta = self.resolve(reference)
        with self._lock:
            if self._documents.get(meta.id) is meta or meta.id in self._documents:
                self._documents.pop(meta.id, None)
                self._by_path.pop(meta.path.lower(), None)
        return meta

    def clear(self) -> None:
        """Remove all registered documents."""
        with self._lock:
            self._documents.clear()
            self._by_path.clear()

    def __len__(self) -> int:
        with self._lock:
            return len(self._documents)

    def __contains__(self, reference: object) -> bool:
        try:
            self.resolve(reference)  # type: ignore[arg-type]
        except (DocumentNotFoundError, ValidationError):
            return False
        return True
