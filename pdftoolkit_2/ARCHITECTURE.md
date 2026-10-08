# Architecture

## Design goals

1. **One application, not three** — the original PDF Toolkit, pdf-search and
   pdfm apps merged into a single engine with one terminology, one result
   contract and one PDF backend.
2. **Headless** — no UI, no Electron, no frontend assumptions. Every output is
   structured, JSON-serialisable data.
3. **Modular but not over-engineered** — four layers with strictly downward
   dependencies; shared logic implemented once.
4. **Fast** — lazy service construction, cached page counts, single-fit PDF
   opens, no redundant parsing, no heavyweight dependencies.

## Layers

```
┌────────────────────────────────────────────────────────────┐
│  api/            cli.py · http.py                          │  adapters
├────────────────────────────────────────────────────────────┤
│  toolkit.py      Toolkit facade (envelopes, strict mode)    │  facade
├────────────────────────────────────────────────────────────┤
│  services/       search · text · links · page_ops ·        │  business
│                  merge · render · speech                   │  logic
├────────────────────────────────────────────────────────────┤
│  documents/      registry (stable ids, page-count cache)   │  shared
│  utils/          files · naming                            │  foundations
│  io/             pdfio (the ONLY PyMuPDF import)           │
├────────────────────────────────────────────────────────────┤
│  core/           errors · result · validation · pages ·     │  pure core
│                  keywords · models                         │
└────────────────────────────────────────────────────────────┘
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

### Consolidations from the original apps
| Original implementation          | Unified into                          |
|----------------------------------|---------------------------------------|
| PyMuPDF + pypdf + PyPDF2 + pdfminer + pdfgrep usage | `io/pdfio.py` (PyMuPDF only) |
| Three different search routines  | `services/search.py` + `core/keywords.py` |
| Ad-hoc page selection in pdfm    | `core/pages.py` (strict spec parser)  |
| Duplicated filename ordering    | `utils/naming.py` (natural sort)      |
| Per-app CLI shapes               | `api/cli.py` (single JSON contract)   |
| pyttsx3 wired into app code      | injectable `SpeechProvider`            |

### Testing strategy
```
tests/unit/        core + utils: pure logic, no filesystem required
tests/integration/ io, registry, services: real PDFs built by tests/helpers.py
tests/e2e/         Toolkit workflows: envelope contract end-to-end
tests/adapters/    CLI + HTTP: envelopes, exit codes, status mapping
```
All tests build their own tiny PDFs with PyMuPDF (no fixtures on disk), so
they are deterministic, isolated and fast. Optional-dependency tests
(FastAPI, pyttsx3) skip cleanly when the extra is not installed.

## Extending the engine

1. Add a module in `services/` deriving from `Service`.
2. Register it in `SERVICE_CATALOG` (`services/__init__.py`).
3. Expose it on `Toolkit` via `_run` + a thin method.
4. (Optional) add the CLI subcommand / HTTP route — both are one-line
   dispatch entries.

No other layer needs to change.
