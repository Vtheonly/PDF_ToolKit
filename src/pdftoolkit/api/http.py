"""FastAPI HTTP adapter (requires the optional ``http`` extra).

Design rules:

* one JSON envelope per response, identical in shape to the CLI/Python
  envelopes;
* the toolkit runs in strict mode so typed errors propagate here and are
  mapped to HTTP statuses via each error's ``http_status``:

  - ``invalid_input`` / ``invalid_page_range`` / ``output_conflict`` /
    ``unreadable_pdf``            -> 400
  - ``document_not_found`` /
    ``content_not_found``          -> 404
  - ``dependency_unavailable``     -> 503
  - anything else                  -> 500

The adapter additionally preserves the upload/download/cleanup workflow of
the original pdf-search Flask server: stateless callers can upload PDFs
(``POST /v1/documents/upload``) into a managed upload directory, download
generated outputs (``GET /v1/files?path=...`` - restricted to the upload
directory) and clean up temporary resources (``DELETE
/v1/documents/{id}?delete_files=true``).
"""

from __future__ import annotations

import os
import re
import uuid
from pathlib import Path
from typing import Any, Dict, Optional, Union

from fastapi import FastAPI, File, Query, Request, UploadFile
from fastapi.responses import FileResponse, JSONResponse

from ..core.errors import PdfToolkitError, ValidationError
from ..core.result import engine_info, failure, from_exception, success
from ..toolkit import Toolkit

SEARCH = "search"
SEARCH_CORPUS = "search_corpus"
EXTRACT_TEXT = "extract_text"
EXTRACT_LINKS = "extract_links"
CUT_PAGES = "cut_pages"
EXTRACT_CONTEXT = "extract_context"
EXTRACT_MATCHES = "extract_matches"
MERGE = "merge"
MERGE_FOLDER = "merge_folder"
RENDER_PAGES = "render_pages"
RENDER_THUMBNAILS = "render_thumbnails"
SYNTHESIZE_SPEECH = "synthesize_speech"

DEFAULT_MAX_UPLOAD_BYTES = 100 * 1024 * 1024  # 100 MB, as the original server
_SAFE_NAME = re.compile(r"[^A-Za-z0-9._-]+")

PathLike = Union[str, "os.PathLike"]


def _safe_name(filename: Optional[str]) -> str:
    """Reduce an uploaded filename to a filesystem-safe token."""
    cleaned = _SAFE_NAME.sub("_", filename or "").strip("._")
    return cleaned or "upload.pdf"


def create_app(
    toolkit: Toolkit = None,
    upload_dir: Optional[PathLike] = None,
    max_upload_bytes: int = DEFAULT_MAX_UPLOAD_BYTES,
) -> FastAPI:
    """Build the FastAPI application around a strict toolkit.

    Args:
        toolkit: engine facade; defaults to a fresh strict ``Toolkit``.
        upload_dir: directory that stores uploads and downloadable outputs.
            Defaults to ``<tempdir>/pdftoolkit-uploads``.
        max_upload_bytes: reject uploads larger than this (default 100 MB).
    """
    app = FastAPI(
        title="pdftoolkit",
        version=engine_info()["version"],
        description="Unified headless PDF engine - every response is a JSON envelope.",
    )
    engine = toolkit if toolkit is not None else Toolkit(strict=True)

    if upload_dir is not None:
        workspace = Path(os.fspath(upload_dir)).expanduser().resolve()
    else:
        workspace = Path(os.path.join(_system_temp(), "pdftoolkit-uploads"))
    workspace.mkdir(parents=True, exist_ok=True)

    def handle(operation: str, action) -> JSONResponse:
        try:
            data = action()
            return JSONResponse(success(operation, data))
        except PdfToolkitError as error:
            return JSONResponse(failure(operation, error), status_code=error.http_status)
        except Exception as error:  # pragma: no cover - defensive boundary
            return JSONResponse(from_exception(operation, error), status_code=500)

    async def body(request: Request) -> Dict[str, Any]:
        try:
            payload = await request.json()
        except Exception:
            return {}
        return payload if isinstance(payload, dict) else {}

    def need(payload: Dict[str, Any], field: str) -> Any:
        if field not in payload or payload[field] is None:
            raise _bad_request(f"missing required field: {field}", {"field": field})
        return payload[field]

    def _bad_request(message: str, details: Dict) -> PdfToolkitError:
        return ValidationError(message, details)

    def _inside_workspace(candidate: Path) -> bool:
        try:
            candidate.relative_to(workspace)
            return True
        except ValueError:
            return False

    @app.get("/health")
    def health() -> Dict[str, Any]:
        return {"ok": True, "engine": engine_info()}

    @app.get("/v1/capabilities")
    def capabilities() -> Any:
        return handle("capabilities", lambda: engine.capabilities()["data"])

    # -- documents -----------------------------------------------------------

    @app.post("/v1/documents/register")
    async def register(request: Request) -> Any:
        payload = await body(request)

        def action() -> Dict:
            return engine.register(need(payload, "path"))["data"]

        return handle("register", action)

    @app.post("/v1/documents/upload")
    async def upload(file: UploadFile = File(...)) -> Any:
        """Store an uploaded PDF in the workspace and register it."""

        def action() -> Dict:
            name = _safe_name(file.filename)
            if not name.lower().endswith(".pdf"):
                raise ValidationError(
                    f"only PDF files are accepted, got: {file.filename!r}",
                    {"filename": file.filename},
                )
            token = uuid.uuid4().hex
            target = workspace / f"{token}_{name}"
            remaining = max_upload_bytes
            with open(target, "wb") as sink:
                while True:
                    chunk = file.file.read(1024 * 1024)
                    if not chunk:
                        break
                    remaining -= len(chunk)
                    if remaining < 0:
                        sink.close()
                        target.unlink(missing_ok=True)
                        raise ValidationError(
                            "uploaded file exceeds the maximum allowed size",
                            {"max_bytes": max_upload_bytes},
                        )
                    sink.write(chunk)
            try:
                meta = engine.registry.register(target)
            except Exception:
                target.unlink(missing_ok=True)
                raise
            return {
                "document": meta.to_dict(),
                "stored_path": str(target),
                "original_name": file.filename,
                "size_bytes": target.stat().st_size,
            }

        return handle("register", action)

    @app.get("/v1/documents")
    def list_documents() -> Any:
        return handle("list_documents", lambda: engine.list_documents()["data"])

    @app.get("/v1/documents/{document_id}")
    def document_info(document_id: str) -> Any:
        return handle("document_info", lambda: engine.document_info(document_id)["data"])

    @app.delete("/v1/documents/{document_id}")
    def remove_document(
        document_id: str,
        delete_files: bool = Query(False, description="also delete uploaded files"),
    ) -> Any:
        """Deregister a document and optionally delete its stored upload."""

        def action() -> Dict:
            meta = engine.registry.remove(document_id)
            deleted: Optional[str] = None
            if delete_files:
                stored = Path(meta.path)
                if _inside_workspace(stored) and stored.is_file():
                    stored.unlink()
                    deleted = str(stored)
            result = {"removed": meta.id, "filename": meta.filename}
            if deleted is not None:
                result["deleted_file"] = deleted
            return result

        return handle("remove_document", action)

    # -- files ---------------------------------------------------------------

    @app.get("/v1/files")
    def download(path: str = Query(..., description="path under the upload directory")) -> Any:
        """Serve a file from the managed upload directory."""
        resolved = Path(path).expanduser().resolve()
        if not _inside_workspace(resolved):
            return JSONResponse(
                failure(
                    "download",
                    ValidationError(
                        "path is outside the managed upload directory",
                        {"path": path, "upload_dir": str(workspace)},
                    ),
                ),
                status_code=403,
            )
        if not resolved.is_file():
            return JSONResponse(
                failure(
                    "download",
                    ValidationError("file not found", {"path": str(resolved)}),
                ),
                status_code=404,
            )
        return FileResponse(resolved)

    # -- search ----------------------------------------------------------------

    @app.post("/v1/search")
    async def search(request: Request) -> Any:
        payload = await body(request)

        def action() -> Dict:
            return engine.search(
                need(payload, "document"),
                need(payload, "keywords"),
                pages=payload.get("pages"),
                case_sensitive=bool(payload.get("case_sensitive", False)),
                whole_words=bool(payload.get("whole_words", True)),
            )["data"]

        return handle(SEARCH, action)

    @app.post("/v1/search/corpus")
    async def search_corpus(request: Request) -> Any:
        payload = await body(request)

        def action() -> Dict:
            return engine.search_corpus(
                need(payload, "keywords"),
                directory=payload.get("directory"),
                pages=payload.get("pages"),
                case_sensitive=bool(payload.get("case_sensitive", False)),
                whole_words=bool(payload.get("whole_words", True)),
                recursive=bool(payload.get("recursive", False)),
                sort=payload.get("sort", "path"),
            )["data"]

        return handle(SEARCH_CORPUS, action)

    # -- extraction --------------------------------------------------------------

    @app.post("/v1/extract/text")
    async def extract_text(request: Request) -> Any:
        payload = await body(request)

        def action() -> Dict:
            return engine.extract_text(need(payload, "document"), pages=payload.get("pages"))["data"]

        return handle(EXTRACT_TEXT, action)

    @app.post("/v1/extract/links")
    async def extract_links(request: Request) -> Any:
        payload = await body(request)

        def action() -> Dict:
            return engine.extract_links(need(payload, "document"), pages=payload.get("pages"))["data"]

        return handle(EXTRACT_LINKS, action)

    # -- pages --------------------------------------------------------------------

    @app.post("/v1/pages/cut")
    async def cut_pages(request: Request) -> Any:
        payload = await body(request)

        def action() -> Dict:
            return engine.cut_pages(
                need(payload, "document"), need(payload, "pages"), need(payload, "output")
            )["data"]

        return handle(CUT_PAGES, action)

    @app.post("/v1/pages/context")
    async def context_pages(request: Request) -> Any:
        payload = await body(request)

        def action() -> Dict:
            return engine.context_pages(
                need(payload, "document"),
                need(payload, "anchor"),
                before=payload.get("before", 1),
                after=payload.get("after", 1),
            )["data"]

        return handle("context_pages", action)

    @app.post("/v1/pages/extract-context")
    async def extract_context(request: Request) -> Any:
        payload = await body(request)

        def action() -> Dict:
            return engine.extract_context(
                need(payload, "document"),
                need(payload, "anchor"),
                need(payload, "output"),
                before=payload.get("before", 1),
                after=payload.get("after", 1),
            )["data"]

        return handle(EXTRACT_CONTEXT, action)

    @app.post("/v1/pages/extract-matches")
    async def extract_matches(request: Request) -> Any:
        payload = await body(request)

        def action() -> Dict:
            return engine.extract_matches(
                need(payload, "document"),
                need(payload, "keywords"),
                need(payload, "output"),
                padding=payload.get("padding", 2),
                pages=payload.get("pages"),
                case_sensitive=bool(payload.get("case_sensitive", False)),
                whole_words=bool(payload.get("whole_words", True)),
            )["data"]

        return handle(EXTRACT_MATCHES, action)

    # -- merge ----------------------------------------------------------------------

    @app.post("/v1/merge")
    async def merge(request: Request) -> Any:
        payload = await body(request)

        def action() -> Dict:
            return engine.merge(need(payload, "inputs"), need(payload, "output"))["data"]

        return handle(MERGE, action)

    @app.post("/v1/merge/folder")
    async def merge_folder(request: Request) -> Any:
        payload = await body(request)

        def action() -> Dict:
            return engine.merge_folder(
                need(payload, "directory"),
                output=payload.get("output"),
                recursive=bool(payload.get("recursive", False)),
                numeric_prefix=bool(payload.get("numeric_prefix", False)),
            )["data"]

        return handle(MERGE_FOLDER, action)

    # -- render -------------------------------------------------------------------------

    @app.post("/v1/render/pages")
    async def render_pages(request: Request) -> Any:
        payload = await body(request)

        def action() -> Dict:
            return engine.render_pages(
                need(payload, "document"),
                pages=payload.get("pages"),
                dpi=payload.get("dpi", 150),
            )["data"]

        return handle(RENDER_PAGES, action)

    @app.post("/v1/render/thumbnails")
    async def render_thumbnails(request: Request) -> Any:
        payload = await body(request)

        def action() -> Dict:
            return engine.render_thumbnails(
                need(payload, "document"),
                pages=payload.get("pages"),
                scale=payload.get("scale", 0.3),
            )["data"]

        return handle(RENDER_THUMBNAILS, action)

    # -- speech ---------------------------------------------------------------------------

    @app.post("/v1/speech/synthesize")
    async def synthesize_speech(request: Request) -> Any:
        payload = await body(request)

        def action() -> Dict:
            return engine.synthesize_speech(
                need(payload, "text"),
                save_path=payload.get("save_path"),
                speak_aloud=bool(payload.get("speak_aloud", False)),
                rate=payload.get("rate"),
                volume=payload.get("volume"),
            )["data"]

        return handle(SYNTHESIZE_SPEECH, action)

    return app


def _system_temp() -> str:
    import tempfile

    return tempfile.gettempdir()


__all__ = ["create_app"]
