# Python Engine — Verified Architecture Map (v2.0.0)

Status: **production, authoritative engine**. 644 tests, ~97% coverage.
Verified line-by-line in session 1 (2026-10-09).

## Layer graph (verified import edges)

```
                 ┌──────────────────────────────────────────────┐
   adapters      │  api/cli.py        api/http.py               │
                 └───────────────┬──────────────────────────────┘
                                 │ imports
                 ┌───────────────▼──────────────────────────────┐
   facade        │  toolkit.py  (Toolkit: _run envelopes,        │
                 │  lazy service cache, strict/non-strict)       │
                 └───────────────┬──────────────────────────────┘
                                 │ imports
                 ┌───────────────▼──────────────────────────────┐
   business      │  services/: base, search, text, links,        │
                 │  page_ops, merge, render, speech              │
                 └──┬──────────────┬──────────────┬──────────────┘
                    │              │              │
       ┌────────────▼───┐  ┌───────▼────────┐  ┌──▼────────────────┐
       │ documents/      │  │ io/pdfio.py    │  │ core/ + utils/    │
       │ registry.py     │  │ (ONLY fitz     │  │ errors, result,   │
       │ (thread-safe,   │  │  import;       │  │ validation, pages,│
       │  stable ids)    │  │  open/merge/   │  │ keywords, models; │
       └────────┬────────┘  │  select)       │  │ files, naming     │
                │           └───────┬────────┘  └───────────────────┘
                └───────────► core/ ◄┘
```

Verified properties (each locked by tests):

* `io/pdfio.py` is the **only** `import fitz` site (backend swap = one file).
* Envelopes `{ok, operation, data, error, engine}` are built **only** in
  `Toolkit._run`; services return plain dicts + raise typed errors.
* `services/__init__.py::SERVICE_CATALOG` is the capability source of truth;
  `build_services()` wires all seven services against one shared
  `DocumentRegistry`.
* `DocumentRegistry` does the expensive PDF open **outside** the RLock and
  re-checks inside it (atomic registration, no lock-held IO).

## Key data flows

### Search (single document) — `services/search.py::SearchService.search`

```
resolve(reference) → open_pdf (single fitz open)
  → per page: page_text → count_keywords (K regex scans per page) ★
  → per page: keywords_with_matches, first_match_snippet
  → aggregate: totals, page_matches (pdf-search MatchResult contract), hits
```
★ = the P-003 hot spot: K×P unindexed regex scans. Python-level fix is
impossible without semantic change (ADR-0004); native fix is issue #1/4.1–4.2.

### Registry — `documents/registry.py::DocumentRegistry.register`

```
resolve_path → check _by_path cache (RLock) → [miss]
  → open_pdf + read page_count (NO lock held) → re-check (RLock)
  → assign doc-N id → store meta + path map
```

### Envelope — `toolkit.py::Toolkit._run`

```
strict=True:  success(op, action())           — errors propagate (HTTP maps code→status)
strict=False: try success(...) except PdfToolkitError → failure(...) except Exception → from_exception(...)
```

## Test anatomy (why the suite is hermetic)

* `tests/helpers.py` builds real PDFs via PyMuPDF at test time
  (`make_pdf`, `make_links_pdf`, `make_corrupt_pdf`, `FakeSpeechProvider`)
  — no binary fixtures on disk, deterministic, isolated.
* Four tiers: `unit/` (pure core), `integration/` (io/registry/services on
  real generated PDFs), `e2e/` (Toolkit workflows incl. every original
  application's workflow), `adapters/` (CLI exit codes, HTTP status
  mapping, uploads).
* Environment sensitivity: without the `http` extra, `test_http.py`
  currently **errors at collection** (P-007, fix pending T-004);
  `test_http_upload.py` skips cleanly via `pytest.importorskip`.

## Frozen contracts (do not break)

* Error taxonomy `core/errors.py`: 8 classes, stable `code` + `http_status`.
* Page-spec grammar `core/pages.py`: strict, `"0"`/`"all"` = all pages,
  inverted/empty/out-of-range are errors, inner whitespace in tokens allowed.
* Match axes `core/keywords.py`: `case_sensitive` (default False) ×
  `whole_words` (default True); per-keyword **non-overlapping** counts.
* Hardening rules (README "Deliberate hardening") are specifications, not
  bugs — every one has a regression test.
