# pdftoolkit — Unified Headless PDF Engine

`pdftoolkit` merges the original **PDF Toolkit**, **pdf-search** and **pdfm**
applications into **one** backend engine with a single architecture, a single
JSON result contract and a single PDF backend. There is **no UI layer** — no
HTML, CSS, Electron or desktop shell. The engine is consumed programmatically
by any frontend through three interchangeable adapters:

* **Python API** — `Toolkit` facade
* **CLI** — `python -m pdftoolkit` (JSON in, JSON out, exit code 0/1)
* **HTTP** — optional FastAPI adapter (`pip install 'pdftoolkit[http]'`)

## Quick start

```bash
pip install -e ".[dev]"     # engine + test tooling
pytest                      # run the full test suite
```

```python
from pdftoolkit import Toolkit

engine = Toolkit()
engine.register("report.pdf")
result = engine.search("report.pdf", ["invoice", "total"])
print(result["data"]["totals"])

merged = engine.merge_folder("invoices/")          # -> invoices/merged.pdf
context = engine.context_pages("report.pdf", "invoice", before=1, after=1)
```

## The result envelope

Every operation — across all three adapters — returns exactly one shape:

```json
{
  "ok": true,
  "operation": "search",
  "data": { "...": "..." },
  "error": null,
  "engine": { "name": "pdftoolkit", "version": "1.0.0" }
}
```

On failure `ok` is `false`, `data` is `null` and `error` carries a stable
machine-readable `code`, a `message` and structured `details`. `strict=True`
(the mode the HTTP adapter uses) raises the same typed errors instead of
wrapping them.

| Error class                  | `code`                   | HTTP |
|------------------------------|--------------------------|------|
| `ValidationError`            | `invalid_input`          | 400  |
| `PageRangeError`             | `invalid_page_range`     | 400  |
| `OutputConflictError`        | `output_conflict`        | 400  |
| `PdfReadError`               | `unreadable_pdf`         | 400  |
| `DocumentNotFoundError`      | `document_not_found`     | 404  |
| `ContentNotFoundError`       | `content_not_found`      | 404  |
| `DependencyUnavailableError` | `dependency_unavailable` | 503  |
| `OperationError`             | `operation_failed`       | 500  |

## Capabilities

| Area      | Operations |
|-----------|------------|
| Documents | `register`, `list_documents`, `document_info`, `remove_document` |
| Search    | `search`, `search_corpus` (per-page hits, snippets, totals) |
| Text      | `extract_text` (all pages or a page spec like `"1-3,7"`) |
| Links     | `extract_links` (page, URI, rectangle) |
| Pages     | `cut_pages`, `context_pages`, `extract_context` |
| Merge     | `merge`, `merge_folder` (natural order, idempotent) |
| Render    | `render_pages` (base64 PNG), `render_to_files` (collision-free) |
| Speech    | `synthesize_speech` (injectable provider, optional extra) |

## Deliberate hardening

The original applications accepted input the new engine rejects on purpose:

* merges require **at least two** readable documents;
* page specs are validated **strictly** (`"0"`/`"all"` = every page; reversed
  ranges, empty tokens and out-of-range pages are errors);
* an output path may **never overwrite one of the operation's own inputs**
  (`output_conflict`);
* **0-page / junk PDFs are unreadable** — PyMuPDF silently "repairs" junk
  files into empty documents, so the engine refuses them explicitly;
* `merge_folder` excludes its own default output (`merged.pdf`) from the
  candidate set, making reruns **idempotent**; pointing the explicit output at
  one of the candidates raises `output_conflict`;
* speech synthesis requires **exactly one output mode** (`save_path` XOR
  `speak_aloud`), never both and never neither.

## License note

PyMuPDF is licensed under **AGPL-3.0**. Shipping this engine (or any service
that links it) must comply with the AGPL terms; swap `io/pdfio.py` for a
permissively-licensed backend if that is a problem for your distribution.

## Project layout

```
src/pdftoolkit/
├── core/        errors, result envelope, validation, page specs, keywords, models
├── io/          the single PyMuPDF-backed PDF backend
├── documents/   thread-safe document registry (stable ids, cached page counts)
├── utils/       file discovery, natural sorting, output naming
├── services/    one module per capability (search, text, links, pages, merge, render, speech)
├── toolkit.py   the facade every adapter talks to
└── api/         cli.py (argparse) and http.py (FastAPI) adapters
tests/
├── unit/  integration/  e2e/  adapters/
```

See `ARCHITECTURE.md` for the full layer diagram and dependency rules.
