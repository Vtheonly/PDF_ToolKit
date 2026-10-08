# pdftoolkit — Unified Headless PDF Engine

`pdftoolkit` is the single, unified PDF engine of this repository. It merges
what used to be **five separate applications** —

| Original application | What it contributed |
|----------------------|----------------------|
| `pdfm` | numeric-prefix (`N_name.pdf`) folder merges |
| `pdf-search` | keyword search with case sensitivity, padding windows, page previews (data-URI thumbnails), upload/search/export workflow |
| `pdftoolkit` (integration folder) | duplicate staging copy of `pdfm` + `pdf-search` |
| `PDF Toolkit` | link extraction, recursive corpus search with per-file ranking, PDF→PNG rendering, text extraction, text-to-speech with rate/volume, multi-range page cutting |
| `pdftoolkit_2` | a previous partial unification attempt (layered architecture, JSON envelope, typed errors, registry, tests) |

— into **one** backend engine with a single architecture, a single JSON
result contract, one terminology and **one PDF backend (PyMuPDF)**, replacing
the former PyMuPDF / pypdf / PyPDF2 / pdfminer / pdfgrep mix. There is **no
UI layer** — no HTML, CSS, Electron or desktop shell. The engine is consumed
programmatically by any application through three interchangeable adapters:

* **Python API** — `Toolkit` facade
* **CLI** — `python -m pdftoolkit` (JSON in, JSON out, exit code 0/1)
* **HTTP** — optional FastAPI adapter (`pip install 'pdftoolkit[http]'`),
  including upload / download / cleanup endpoints for stateless callers

## Quick start

```bash
pip install -e ".[dev]"     # engine + test tooling
pytest                      # run the full test suite (644 tests)
```

```python
from pdftoolkit import Toolkit

engine = Toolkit()
engine.register("report.pdf")
result = engine.search("report.pdf", ["invoice", "total"])
print(result["data"]["totals"])

merged = engine.merge_folder("invoices/")                     # -> invoices/merged.pdf
chapters = engine.merge_folder("book/", numeric_prefix=True)  # pdfm behaviour
context = engine.context_pages("report.pdf", "invoice", before=1, after=1)
filtered = engine.extract_matches(                             # pdf-search workflow
    "report.pdf", ["invoice"], "filtered.pdf", padding=2
)
ranked = engine.search_corpus(["lex"], directory="docs/", recursive=True, sort="rank")
thumbs = engine.render_thumbnails("report.pdf", pages="1-3", scale=0.3)
engine.synthesize_speech("invoice ready", save_path="notice.wav", rate=200, volume=1.0)
```

## The result envelope

Every operation — across all three adapters — returns exactly one shape:

```json
{
  "ok": true,
  "operation": "search",
  "data": { "...": "..." },
  "error": null,
  "engine": { "name": "pdftoolkit", "version": "2.0.0" }
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
| Documents | `register`, `list_documents`, `document_info`, `remove_document` (+ HTTP `upload`, file download, cleanup) |
| Search    | `search`, `search_corpus` — per-page hits, per-page match summaries (`keywords_found`), snippets, totals, optional case sensitivity, optional substring (whole-word default), recursive folder scans, relevance ranking (`sort="rank"`) |
| Text      | `extract_text` (all pages or a page spec like `"1-3,7"`) |
| Links     | `extract_links` (page, URI, rectangle) |
| Pages     | `cut_pages` (multi-range plans like `"19-22,29-31"`), `context_pages`, `extract_context`, `extract_matches` (search + padding window + export in one call) |
| Merge     | `merge`, `merge_folder` (natural order, idempotent, optional `numeric_prefix` mode, optional recursive) |
| Render    | `render_pages` (base64 PNG at DPI), `render_thumbnails` (scale factor, `data:image/png;base64` URIs), `render_to_files` (collision-free) |
| Speech    | `synthesize_speech` (injectable provider, optional `rate` / `volume`, optional extra) |

## Deliberate hardening

The original applications accepted input the unified engine rejects on
purpose:

* merges require **at least two** readable documents;
* page specs are validated **strictly** (`"0"`/`"all"` = every page;
  reversed ranges, empty tokens and out-of-range pages are errors);
* an output path may **never overwrite one of the operation's own inputs**
  (`output_conflict`);
* **0-page / junk / encrypted PDFs are unreadable** — PyMuPDF silently
  "repairs" junk files into empty documents, so the engine refuses them
  explicitly;
* `merge_folder` always excludes the folder's own `merged.pdf` from the
  candidate set, making reruns **idempotent**; pointing an explicit output
  at one of the remaining candidates raises `output_conflict`;
* speech synthesis requires **exactly one output mode** (`save_path` XOR
  `speak_aloud`), never both and never neither;
* HTTP uploads are extension-checked, size-capped (100 MB default),
  stored under a managed upload directory, and downloads are restricted to
  that directory — path traversal is rejected with 403.

## Performance notes

The engine is built for efficiency: lazy service construction, cached page
counts (read once at registration), single-fit document opens per
operation, one-shot workflows that never re-parse pages (`extract_matches`
scans the document once), reusable selection/merge primitives in
`io/pdfio.py`, streaming HTTP uploads (1 MB chunks), and immediate cleanup
of temporary resources on failure.

## Native core (in progress)

A C++20 native core is being built under `native/` per
[issue #1](https://github.com/Vtheonly/PDF_ToolKit/issues/1) — the
re-architecture from Python wrapper to high-throughput systems engine.
The Python engine above remains the authoritative implementation until
the native core reaches parity. Current state: Phase 0 complete; the
Phase 1 memory subsystem (tasks 1.1 guarded `MmapHandle`, 1.2
`BumpArena`, 1.3 Unified Page Slab) done and measured.

```bash
cmake --preset release -DPython3_EXECUTABLE=$(which python)  # needs cmake+ninja
cmake --build --preset release   # targets: core, ffi, py extension, cli + benchmarks
ctest --preset release           # native test suite
./build/release/native/pdftoolkit_cli --version

scripts/run_perf.sh              # automated benchmarks (+ perf stat when perf exists;
                                  # SKIPPED honestly otherwise, see docs/recovery U-010)
```

Offline builds skip the FetchContent benchmark suite with
`-DPDTK_ENABLE_BENCHMARKS=OFF` (policy: ADR-0005). Recorded results — the
only citable performance numbers — live in `docs/benchmarks/`; progress,
decisions and verification evidence live in
[`docs/recovery/task-registry.md`](docs/recovery/task-registry.md).

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
