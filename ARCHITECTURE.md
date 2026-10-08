# Architecture

## Design goals

1. **One application, not five** — the original pdfm, pdf-search, pdftoolkit,
   PDF Toolkit and pdftoolkit_2 applications merged into a single engine with
   one terminology, one result contract and one PDF backend.
2. **Headless** — no UI, no Electron, no frontend assumptions. Every output is
   structured, JSON-serialisable data.
3. **Modular but not over-engineered** — four layers with strictly downward
   dependencies; shared logic implemented once.
4. **Fast** — lazy service construction, cached page counts, single-fit PDF
   opens, no redundant parsing, no heavyweight dependencies.

## Layers

```
┌────────────────────────────────────────────────────────────────┐
│  api/            cli.py · http.py (+ upload/download/cleanup)  │  adapters
├────────────────────────────────────────────────────────────────┤
│  toolkit.py      Toolkit facade (envelopes, strict mode)        │  facade
├────────────────────────────────────────────────────────────────┤
│  services/       search · text · links · page_ops ·            │  business
│                  merge · render · speech                       │  logic
├────────────────────────────────────────────────────────────────┤
│  documents/      registry (stable ids, page-count cache)       │  shared
│  utils/          files · naming                                │  foundations
│  io/             pdfio (the ONLY PyMuPDF import)               │
├────────────────────────────────────────────────────────────────┤
│  core/           errors · result · validation · pages ·         │  pure core
│                  keywords · models                             │
└────────────────────────────────────────────────────────────────┘
```

**Dependency rule:** a layer may only import from layers below it. Nothing
imports from `api/` or `toolkit.py`. `io/pdfio.py` is the only module that
imports `fitz` — replacing the PDF backend is a one-file change.

## Key decisions

### One envelope everywhere
`core/result.py` defines `{ok, operation, data, error, engine}`. Services
return plain dicts and raise typed errors; only `Toolkit._run` builds
envelopes, so the contract cannot drift between adapters.

### Strict vs. non-strict
`Toolkit(strict=False)` (default) returns failure envelopes — ideal for
scripting. `Toolkit(strict=True)` raises the same typed errors — ideal for
the HTTP adapter, which maps `error.http_status` to response codes.

### Document registry
`documents/registry.py` gives documents stable ids (`doc-1`, ...) with
auto-registration of unknown-but-existing paths, so both stateless (CLI/HTTP)
and session-style (Python) callers share one interface. Registration is
atomic: unreadable PDFs never enter the registry. Page counts are read once
and cached; the registry is thread-safe.

### Service layer
Each service owns one capability and depends only on the registry, `io/`
and `core/`. The catalog in `services/__init__.py` is the single source of
truth surfaced by `capabilities()`. Services never touch JSON envelopes.

### Consolidations from the original applications

| Original implementation                              | Unified into                                   |
|------------------------------------------------------|-----------------------------------------------|
| PyMuPDF + pypdf + PyPDF2 + pdfminer + pdfgrep usage  | `io/pdfio.py` (PyMuPDF only, no external binaries) |
| pdf-search substring matching / pdftoolkit word-boundary matching | `core/keywords.py` — `case_sensitive` + `whole_words` axes |
| pdf-search `MatchResult` summaries                   | `page_matches` / `matched_pages` in `services/search.py` |
| pdfgrep recursive scans + occurrence ranking         | `search_corpus(recursive=True, sort="rank")`, native |
| pdf-search `calculate_page_range` padding            | `core/pages.py::expand_with_padding`          |
| pdf-search search→padding→export server workflow     | `services/page_ops.py::extract_matches` (one shot, single scan) |
| pdf-search `render_page_thumbnail(s)`                | `services/render.py::render_thumbnails` (scale + data URIs) |
| pdf-search upload/preview/export/cleanup HTTP API    | `api/http.py` upload / download / delete_files endpoints |
| pdfm `N_name.pdf` pattern merges                     | `merge_folder(numeric_prefix=True)`           |
| pdfContextCutter multi-range extraction plans        | `cut_pages` page specs (`"19-22,29-31"`)      |
| textToSpeach rate/volume pyttsx3 properties          | `synthesize_speech(rate=..., volume=...)`     |
| Three different search routines                      | `services/search.py` + `core/keywords.py`     |
| Ad-hoc page selection in pdfm                        | `core/pages.py` (strict spec parser)          |
| Duplicated filename ordering                         | `utils/naming.py` (natural sort, type-safe)   |
| Per-app CLI shapes                                   | `api/cli.py` (single JSON contract)           |
| pyttsx3 wired into app code                          | injectable `SpeechProvider`                   |

### Bugs fixed during the unification

The previous unification attempt (`pdftoolkit_2`) shipped with defects that
its own (never-green) test suite documented; all are fixed and now locked in
by regression tests:

* `TextService` referenced `parse_page_spec` without importing it — every
  `extract_text` call raised `NameError`;
* the HTTP `/v1/capabilities` route passed a dict where a callable was
  expected — the endpoint always returned 500;
* `render_to_files` created the *parent* of the output directory but not the
  directory itself;
* `natural_key` returned empty-string chunks (`natural_key("42")` was
  `['', 42, '']`) and mixed-type list comparisons could raise `TypeError`
  during natural sorting;
* page-spec range tokens with inner whitespace (`"3 - 4"`) were rejected;
* `merge_folder` with an explicit external output still merged a stale
  `merged.pdf` lying in the source folder;
* `render_filename` produced names that contradicted the documented
  `{prefix}-page-NNNN.png` contract;
* opening a missing file raised `ValidationError` instead of `PdfReadError`.

### Testing strategy
```
tests/unit/        core + utils: pure logic, no filesystem required
tests/integration/ io, registry, services: real PDFs built by tests/helpers.py
tests/e2e/         Toolkit workflows incl. every original application's workflow
tests/adapters/    CLI + HTTP: envelopes, exit codes, status mapping, uploads
```
All tests build their own tiny PDFs with PyMuPDF (no fixtures on disk), so
they are deterministic, isolated and fast. Optional-dependency tests
(FastAPI, pyttsx3) skip cleanly when the extra is not installed (fixed via
`pytest.importorskip`, problem P-007 / task T-004 in `docs/recovery/`).
The suite (644 tests, verified via
`pytest --collect-only -q`) covers ~97% of the engine; the only uncovered block is the real
pyttsx3 engine wrapper, which requires audio hardware.

## Extending the engine

1. Add a module in `services/` deriving from `Service`.
2. Register it in `SERVICE_CATALOG` (`services/__init__.py`).
3. Expose it on `Toolkit` via `_run` + a thin method.
4. (Optional) add the CLI subcommand / HTTP route — both are one-line
   dispatch entries.

No other layer needs to change.
