# Problem Registry — Bugs, Architectural Problems, Technical Debt, Limitations

Every entry carries a stable ID (`P-NNN`), status, and enough context for a
future agent to act without re-investigation. Resolved entries keep their
history — do not delete them.

---

## P-001 — The Delegation Defect (architecture)

* **Where:** `src/pdftoolkit/io/pdfio.py` (the only PyMuPDF import).
* **What:** The engine implements zero PDF parsing, zero stream
  decompression, zero font processing; 100% of byte traversal is delegated
  to MuPDF's shared library. It cannot control memory layout, allocation
  arenas or thread scheduling.
* **Why it matters / Root cause:** The engine is a Python glue wrapper, not
  a systems engine (audit `docs/issues.md` §1.1 defect 1).
* **Status:** `open` — being resolved by issue #1 Phase 1–3 (native core).
* **Fix approach:** Native C++20 parser stack (`native/src/parser/`,
  `native/src/codec/`, `native/src/font/`); Python engine stays
  authoritative until parity (AGENTS.md §5.6).

## P-002 — The Memory Wall Defect (architecture)

* **Where:** `src/pdftoolkit/services/text.py`, `services/search.py`,
  `io/pdfio.py::PdfDocument.extract_text`.
* **What:** Page text is materialized as CPython `PyUnicode` objects
  (48–56 byte headers, scattered heap allocations) stored in
  `dict[str, Any]` structures → deep pointer chasing, ~0% cache locality.
* **Status:** `open` — resolved by the Unified Page Slab (issue #1 Tasks
  1.3, 3.3) and Buffer-Protocol export (7.2).
* **Do NOT attempt** a Python-level mitigation (e.g. bytes/array
  containers): it would not change the ownership model and would create
  drift; see ADR-0002 for the isolation principle.

## P-003 — Algorithmic Brute-Force Search (architecture)

* **Where:** `src/pdftoolkit/core/keywords.py::count_keywords`.
* **What:** K keywords × P pages unindexed regex scans (10 keywords over a
  1,000-page document = 10,000 `re.findall` passes over heap strings).
* **Status:** `open` — resolved by the native inverted index + Block-Max
  WAND (issue #1 Tasks 4.1, 4.2).
* **Critical constraint discovered:** a Python-level "combine keywords into
  one alternation regex" optimization is **semantically impossible** —
  leftmost-first alternation breaks substring-mode counts for overlapping
  keywords, and lookahead scanning breaks per-keyword non-overlap. See
  ADR-0004; do not retry this.
* **Note:** `re.compile` results are internally cached by CPython's `re`
  module, so pattern compilation is not the bottleneck — scanning is.

## P-004 — GIL Serialization of the Registry (architecture)

* **Where:** `src/pdftoolkit/documents/registry.py`
  (`threading.RLock()`-synchronized dict registry).
* **What:** Under concurrent server load, CPython's GIL serializes CPU
  execution to one core; multi-core scaling is impossible.
* **Status:** `open` — resolved by the native lock-free registry and
  work-stealing pool (issue #1 Tasks 6.1, 6.2).
* **Mitigation already present (do not remove):** the expensive part of
  registration (opening the PDF) happens **outside** the lock, so file IO
  does not serialize; only the dict mutations are lock-guarded.

## P-005 — Envelope Marshalling Thrash (architecture)

* **Where:** `src/pdftoolkit/core/result.py`, `api/http.py`,
  `api/cli.py`.
* **What:** Native C data → PyObject strings/dicts → envelope dict → JSON
  string → socket: four materialization stages per response.
* **Status:** `open` — resolved by C-ABI + buffer-protocol zero-copy path
  (issue #1 Tasks 7.1, 7.2) and native CLI (7.3).

## P-006 — Stale test-count claims in README and ARCHITECTURE (docs bug)

* **Where:** `README.md` ("646 tests"), `ARCHITECTURE.md` ("The suite
  (646 tests)").
* **What:** The suite collects **644** tests; the documented count had
  drifted by 2 without a doc update.
* **Root cause:** Tests were removed/merged in an earlier commit without
  updating the prose (exact commit unidentified — see U-001).
* **Verified:** `.venv/bin/python -m pytest --collect-only -q` →
  `644 tests collected in 0.74s`; full run → `644 passed, 1 warning`.
* **Status:** `resolved` — counts corrected in this commit; rule added to
  `dos-and-donts.md` (re-measure, never restate stale counts).

## P-007 — HTTP adapter tests abort collection when `http` extra absent (test bug)

* **Where:** `tests/adapters/test_http.py`.
* **What:** The module docstring and `ARCHITECTURE.md` claim the tests
  "skip cleanly when the extra is not installed", but the module imports
  `fastapi.testclient` at module level guarded only by
  `pytestmark = pytest.mark.skipif(...)`. `skipif` cannot prevent a
  module-import failure: without fastapi the suite **errors at collection
  and aborts** (`Interrupted: 1 error during collection`) instead of
  skipping.
* **Reproduced:** 2026-10-09 on a `.[dev]`-only install →
  `ModuleNotFoundError: No module named 'fastapi'`, `1 skipped, 1 error`.
  The sibling `test_http_upload.py` uses the correct
  `pytest.importorskip("fastapi")` pattern and skips cleanly (that was the
  "1 skipped").
* **Fix:** use `pytest.importorskip("fastapi")` (+ httpx) before the
  fastapi imports, exactly like `test_http_upload.py`.
* **Status:** `open` — task T-004 in the task registry.

## P-008 — MemorySanitizer unverifiable in the local environment (limitation)

* **What:** MSan (`-fsanitize=memory`) requires clang (GCC does not
  implement it). The reference environment has only GCC 14.2.0.
* **Impact:** The audit's Task 0.3 MSan preset cannot be locally verified;
  it must run in CI with a clang toolchain.
* **Status:** `open` (CI-only verification) — see U-004.

## P-009 — AGPL-3.0 licensing of the PDF backend (distribution constraint)

* **What:** PyMuPDF is AGPL-3.0; shipping the Python engine (or any
  service linking it) must comply. The native C++ core that does not link
  MuPDF is not AGPL-bound by itself.
* **Status:** `open` (standing constraint) — README "License note" is the
  canonical statement; keep the boundary documented if the native core
  ever links MuPDF.

## P-010 — Optional extras change suite outcome shape (environment pitfall)

* **What:** The baseline suite requires `pip install -e ".[dev,http]"` for
  a fully green run. With only `.[dev]`, the run aborts (see P-007). With
  `.[all]` (adds pyttsx3), the real speech engine tests run and need audio
  hardware (documented in ARCHITECTURE.md as the only uncovered block).
* **Status:** `open` (accepted pitfall; P-007's fix reduces it to a clean
  skip). Recorded so the next agent does not misread a collection error as
  engine breakage.
