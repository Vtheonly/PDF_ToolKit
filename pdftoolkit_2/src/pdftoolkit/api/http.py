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
"""

from __future__ import annotations

from typing import Any, Dict

from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse

from ..core.errors import PdfToolkitError, ValidationError
from ..core.result import engine_info, failure, from_exception, success
from ..toolkit import Toolkit

SEARCH = "search"
EXTRACT_TEXT = "extract_text"
EXTRACT_LINKS = "extract_links"
CUT_PAGES = "cut_pages"
EXTRACT_CONTEXT = "extract_context"
MERGE = "merge"
MERGE_FOLDER = "merge_folder"
RENDER_PAGES = "render_pages"
SYNTHESIZE_SPEECH = "synthesize_speech"


def create_app(toolkit: Toolkit = None) -> FastAPI:
    """Build the FastAPI application around a strict toolkit."""
    app = FastAPI(
        title="pdftoolkit",
        version=engine_info()["version"],
        description="Unified headless PDF engine - every response is a JSON envelope.",
    )
    engine = toolkit if toolkit is not None else Toolkit(strict=True)

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

    @app.get("/health")
    def health() -> Dict[str, Any]:
        return {"ok": True, "engine": engine_info()}

    @app.get("/v1/capabilities")
    def capabilities() -> Any:
        return handle("capabilities", engine.capabilities()["data"])

    @app.post("/v1/documents/register")
    async def register(request: Request) -> Any:
        payload = await body(request)

        def action() -> Dict:
            return engine.register(need(payload, "path"))["data"]

        return handle("register", action)

    @app.get("/v1/documents")
    def list_documents() -> Any:
        return handle("list_documents", lambda: engine.list_documents()["data"])

    @app.get("/v1/documents/{document_id}")
    def document_info(document_id: str) -> Any:
        return handle("document_info", lambda: engine.document_info(document_id)["data"])

    @app.delete("/v1/documents/{document_id}")
    def remove_document(document_id: str) -> Any:
        return handle("remove_document", lambda: engine.remove_document(document_id)["data"])

    @app.post("/v1/search")
    async def search(request: Request) -> Any:
        payload = await body(request)

        def action() -> Dict:
            return engine.search(
                need(payload, "document"),
                need(payload, "keywords"),
                pages=payload.get("pages"),
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
            )["data"]

        return handle("search_corpus", action)

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
            )["data"]

        return handle(MERGE_FOLDER, action)

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

    @app.post("/v1/speech/synthesize")
    async def synthesize_speech(request: Request) -> Any:
        payload = await body(request)

        def action() -> Dict:
            return engine.synthesize_speech(
                need(payload, "text"),
                save_path=payload.get("save_path"),
                speak_aloud=bool(payload.get("speak_aloud", False)),
            )["data"]

        return handle(SYNTHESIZE_SPEECH, action)

    return app


__all__ = ["create_app"]
