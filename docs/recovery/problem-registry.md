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
* **Status:** `resolved` — fixed in this commit (task T-004; see git log for SHA). Verified in
  both environments:
  * with `http` extra: `pytest` → `644 passed, 1 warning` (no regression;
    per-file counts identical);
  * without `http` extra (clean `.[dev]`-only venv): `pytest` →
    `589 passed, 5 skipped`, exit 0, **no collection error** — the two
    HTTP modules (28 + 24 tests) now skip at module level (2 skip
    entries) and 3 http-specific tests in `test_edge_cases.py` skip at
    function level, matching the documented "skip cleanly" behaviour.

## P-008 — MemorySanitizer unverifiable in the local environment (limitation)

* **What:** MSan (`-fsanitize=memory`) requires clang (GCC does not
  implement it). The reference environment has only GCC 14.2.0.
* **Impact:** The audit's Task 0.3 MSan preset cannot be locally verified;
  it must run in CI with a clang toolchain.
* **Status:** `open` (CI-only verification) — see U-002.

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

## P-012 — Audit task-0.2 acceptance criterion depends on Phase 4 components (audit defect)

* **Where:** `docs/issues.md` (issue #1), Task 0.2 acceptance: "Automated
  execution of `./build/benchmarks/bench_wand` outputs statistical latency
  curves" — and its dependency line: "Dependencies / Prerequisites:
  Task 0.1".
* **What:** the acceptance names four benchmark files (`bench_mmap`,
  `bench_lexer`, `bench_slab`, `bench_wand`), but three of the four measure
  engines that do not exist in Phase 0: `bench_wand` needs the Block-Max
  WAND engine (task 4.2), `bench_lexer` the zero-copy tokenizer (2.3),
  `bench_slab` the page slab (1.3). Only `bench_mmap` (task 1.1) is one
  phase away. The stated prerequisite (0.1 only) is wrong for the
  benchmark set as written — same planning-defect class as P-011
  (acceptance text inconsistent with the plan's own dependencies).
* **Resolution implemented (2026-10-09):** the *pipeline* landed in task
  0.2 (option-gated FetchContent, runner script, percentile method,
  recording policy — ADR-0005); each bench file lands **with the task
  that builds its component** (`bench_arena` closed task 1.2 in the same
  commit; `bench_mmap` arrives with 1.1, `bench_slab` with 1.3,
  `bench_lexer` with 2.3, `bench_wand` with 4.2 — map recorded at the top
  of `native/benchmarks/CMakeLists.txt`). Benchmarks never fake numbers
  for unbuilt engines (the honest-stub principle applied to the pipeline
  itself). Acceptance reinterpreted as: automated execution of the
  *available* benchmark targets outputs statistical latency distributions
  — satisfied since 2026-10-09.
* **Action for the audit owner:** correct task 0.2's dependency line and
  acceptance text in `docs/issues.md` / issue #1, and note the path
  translation `benchmarks/*` → `native/benchmarks/*` (ADR-0002 mapping).
* **Status:** `open` (audit text correction outstanding; code side done —
  the literal `bench_wand` acceptance becomes satisfiable when task 4.2
  lands).

## P-011 — Audit task-1.2 acceptance criterion is physically mis-scaled (audit defect)

* **Where:** `docs/issues.md` (issue #1), Task 1.2 acceptance: "Benchmark
  demonstrating that 1,000,000 slice allocations and a `.reset()` call
  execute in < 30 microseconds total".
* **What:** 30 µs / 1,000,000 allocations = **30 picoseconds per
  allocation** — below the per-operation latency of any existing CPU
  (a single L1-hit instruction is ~100–300 ps; a taken branch alone can
  exceed 30 ps). The criterion cannot be met by any implementation,
  including a perfect bump allocator in hand-written assembly.
* **Verified:** the Phase-0 `BumpArena` measures **423,258 ns**
  (≈ 0.42 ns/allocation, ~1–2 cycles/alloc at -O3) for exactly this
  workload — roughly **14× over** the audit's bound while being, for all
  practical purposes, at the hardware floor. Test:
  `build/native/pdtk_test_arena` (case `million_allocations_and_reset_are_fast`).
* **Root cause (of the audit text):** the criterion was likely drafted as
  "30 µs for 1,000 allocations" or "30 ms" and scaled inconsistently.
* **Resolution:** treat the *intent* (bump allocation ≈ free, O(1) reset)
  as authoritative, not the literal number. The regression guard in the
  test asserts < 10 ms; the authoritative measurement landed with Google
  Benchmark (task 0.2, 2026-10-09): mean **485.48 us**, P50 482.15 /
  P90 496.27 / P99 513.31 us (20 repetitions, CV 2.4%); steady-state
  alloc+reset pair **0.28 ns** — ~16x the audit's literal bound while at
  the hardware floor. Recording:
  `docs/benchmarks/2026-10-09-phase0-arena-baseline.md`.
  **Action for the audit owner:** correct the criterion in
  `docs/issues.md` / issue #1.
* **Status:** `open` (audit text amendment outstanding — the only remaining
  action; the code side is done, measured and guarded).

## P-013 — CI branch filter *appeared* corrupted — terminal rendering artifact (withdrawn; verification pitfall)

* **Where:** `.github/workflows/ci.yml`, `on: push: branches:` — and,
  more importantly, **any future byte-level verification done through
  terminal output**.
* **What happened:** the filter `branches: [main]` was repeatedly
  *displayed* as `branches: ain]` by the tool-output rendering layer,
  which consumed `[m` as if it were an ANSI escape sequence (`ESC[m` =
  reset). A grep for `ain]` even "matched" — because `ain]` is a
  substring of `[main]`. This briefly led to a false "CI trigger typo"
  diagnosis (P-013 as originally drafted) during session 2, including a
  claimed "fix" that had nothing to change: the file was byte-exact
  `    branches: [main]\n` all along (`od -c` proof), and CI had
  triggered on every push to main (3/3 runs green) exactly as configured.
* **Why it is still recorded:** the pitfall is real and will recur.
  Square-bracketed YAML values (`[main]`, `[a, b]`) and ANSI-adjacent
  sequences in tool output can silently alter *what the agent sees*,
  producing confident false positives. This cost real investigation
  time once; the entry prevents a repeat.
* **Rule going forward:** any "corrupted bytes" conclusion drawn from
  rendered output must be confirmed with a byte-level dump (`od -c` /
  `git diff`) before it is written to a registry — and grep patterns
  must not be substrings that can match through the artifact.
* **Status:** `withdrawn` (no defect ever existed; recorded as a
  verification-methodology pitfall).

## P-014 — Audit task-1.1 step 4 prescribes throwing from a signal handler — undefined behaviour (audit defect)

* **Where:** `docs/issues.md` (issue #1), Task 1.1 step 4: "Register a
  thread-safe `sigaction` handler for `SIGBUS` ... and **throw**
  `PdfToolkitException(ErrorCode::IoTruncated)`."
* **What:** throwing a C++ exception from inside a signal handler is
  undefined behaviour (the unwinder can run on the signal stack
  mid-interruption; POSIX provides no exception-safe path out of
  `sigaction`). Same audit-defect class as P-011/P-012: the *intent* is
  sound and implementable, the literal prescription is not.
* **Resolution implemented (2026-10-09):** the typed exception is thrown
  from the normal stack — `guarded()` establishes a `sigsetjmp` recovery
  point; the handler validates the fault against a live registered
  mapping and `siglongjmp()`s back; `guarded()` throws
  `PdfToolkitException(IoTruncated)`. Full design, honesty contract and
  alternatives: **ADR-0006**. The audit's acceptance criterion
  (truncation access → controlled exception, never a crash) is met and
  test-locked (`native/tests/test_mmap.cpp`).
* **Action for the audit owner:** amend task 1.1 step 4 wording in
  `docs/issues.md` / issue #1 (throw *via the guard's recovery path*,
  not *from the handler*).
* **Status:** `open` (audit text amendment outstanding; code side done,
  verified under ASan+UBSan and TSan).
