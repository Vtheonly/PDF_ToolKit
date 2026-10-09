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

## P-018 — Audit task-2.3's 2.5 GB/s throughput gate is unreachable by the scalar design the same task prescribes (audit defect)

* **Where:** `docs/issues.md` (issue #1), Task 2.3 acceptance:
  "Tokenization throughput exceeds 2.5 GB/s on uncompressed streams."
* **What:** the task prescribes a scalar, token-at-a-time lexer
  (256-entry LUT, PdfToken stream) and then gates it at 2.5 GB/s. The
  implemented lexer measures **350.7 MB/s** at 67.7 M tokens/s (~14.7
  ns ≈ 45 cycles/token — consistent with the prescribed design; the
  same-run traversal control measured 4.59 GB/s, so this is not
  primarily an environment ceiling). Meeting the gate at this token
  density needs ~6 cycles/token — SIMD byte classification (32+ bytes
  per instruction), which the audit itself introduces only in task 4.3
  (AVX2/NEON). The gate is scheduled before the technology that can
  satisfy it — same planning-defect class as P-012/P-017a.
* **Verified:** `scripts/run_perf.sh --json build/lexer-reports
  build/release/native/benchmarks/pdtk_bench_lexer` (2026-10-09, 10
  repetitions, percentiles by nearest-rank): content mix 350.7 MB/s
  mean (P50 350.8 / P90 352.6 / P99 353.2, CV 0.6 %); string-heavy
  833.3 MB/s; control 4.586 GB/s. Recording:
  `docs/benchmarks/2026-10-09-phase2-lexer-baseline.md`.
* **Resolution:** the lexer is done and test-locked (18/18 cases; the
  correctness contract is not affected). The GATE is recorded as unmet
  with its blocker; the durable metrics are tokens/s, cycles/token and
  the control ratio. The audit's 800 MB/s Flate gate (task 2.4) is
  reachable and unaffected.
* **Action for the audit owner:** re-scope task 2.3's acceptance
  (tokens/s, or a fraction-of-traversal ratio) or move the GB/s figure
  to the task-4.3 SIMD rewrite of the token stream.
* **Status:** `open` (audit text amendment outstanding; code side done,
  measured and recorded).

## P-017 — Audit task-2.2 has a dependency inversion and an underspecified data structure (audit defects)

* **Where:** `docs/issues.md` (issue #1), Task 2.2.
* **What (a — dependency inversion, same class as P-012):** step 3 asks
  the resolver to "Parse compressed XRef streams (PDF 1.5+ /Type /XRef
  streams with variable-width field decoding)" with the dependency line
  naming only Tasks 2.1 and 1.2 — but real PDF 1.5+ xref streams are
  almost always **FlateDecode-filtered**, and the decompressor arrives
  with Task 2.4 (`libdeflater`). As written, task 2.2's acceptance
  ("correctly parses ... compressed streams") is unsatisfiable for the
  files the feature exists for until 2.4 lands.
* **What (b — underspecified structure):** step 4 mandates
  `std::vector<uint64_t>` indexed by object id, but step 3 requires
  parsing type-2 (compressed, in-ObjStm) entries, which need the
  containing object number AND the index within it — two more fields
  than a uint64 can carry (plus generation for types 0/1).
* **Resolution implemented (2026-10-09; (a) completed by task 2.4):**
  (a) the resolver parses UNFILTERED xref streams fully, and since task
  2.4 a `/FlateDecode` xref stream with a direct `/Length` is
  FIRST-CLASS: the codec decompresses the rows into a lazily created
  scratch `BumpArena` and feeds them to the same row decoder
  (`decode_rows`) — exactly the hand-off this entry predicted. Degradation
  to the emergency linear scan (flagged via `XRefIndex::
  from_linear_scan()`) remains the honest path for: non-Flate filters,
  multi-filter pipelines, predictor-bearing `/DecodeParms` (P-020),
  indirect `/Length`, corrupt payloads, bomb-defense refusals, and
  offline builds (`PDTK_ENABLE_FLATE=OFF`). (b) `std::vector<XRefEntry>`
  (a compact POD with Kind/offset/generation/objstm_object/
  index_in_objstm) — the audit's intent (O(1) id-indexed lookup) is
  preserved, the representation extended to what step 3 requires.
* **Action for the audit owner:** amend task 2.2 in `docs/issues.md` /
  issue #1 — add task 2.4 to the dependency line (or mark Flate streams
  as 2.4 scope) and correct the lookup-table type.
* **Status:** `open` (audit text amendment outstanding; code side fully
  done and test-locked — the 2,200-document corpus now includes 200
  REAL zlib-compressed Flate xref streams verified first-class, plus
  degradation cases for every path above).

## P-016 — Audit task-2.1 hard-codes the backward-scan window at 1024 bytes (audit limitation)

* **Where:** `docs/issues.md` (issue #1), Task 2.1 step 2: "Scan the last
  1024 bytes of the memory-mapped file backward."
* **What:** 1024 bytes is a *tunable*, not a format limit. Real files can
  carry more than 1 KiB of trailing data after the final `%%EOF`
  (appended junk, signatures, some linearization arrangements); with a
  hard 1024-byte window such files are rejected as `UnreadablePdf` even
  though their `startxref` is present and unambiguous further back.
  Production readers make the same trade-off visible: pdf.js slices the
  last 1024 bytes, MuPDF searches deeper.
* **Resolution implemented (2026-10-09):** `locate_startxref` takes the
  window as a parameter (`tail_window`, default 1024 — the audit's
  value), so callers with corpora known to carry heavy trailing junk
  can widen it without code changes. The window edge is test-locked
  (`keyword_exactly_at_window_edge_is_found`,
  `keyword_outside_default_window_needs_larger_window`).
* **Action for the audit owner:** note the tunable in task 2.1's text in
  `docs/issues.md` / issue #1 (the default stays 1024; the acceptance
  corpus stays inside it).
* **Status:** `open` (audit text amendment outstanding; code side done
  and test-locked).

## P-015 — Audit task-1.3's PageSlabHeader cannot satisfy its own static_assert (audit defect)

* **Where:** `docs/issues.md` (issue #1), Task 1.3 step 1: the specified
  `PageSlabHeader` has fourteen 4-byte fields (56 bytes) **plus
  `uint8_t reserved[12]`** = **68 bytes**, while the same snippet demands
  `static_assert(sizeof(PageSlabHeader) == 64)`.
* **What:** 68 > 64 — and with `alignas(64)` the compiler must round the
  struct up to **128** bytes, so the audit's own assertion can never
  pass. Same audit-defect class as P-011/P-012/P-014: the intent (one
  cache line) is right, the literal arithmetic is wrong.
* **Resolution implemented (2026-10-09):** `reserved[8]` instead of
  `reserved[12]` — 56 + 8 = 64 exactly; every named field is kept, in
  the audit's order, with the audit's names. Frozen with
  `static_assert`s on `sizeof`, `alignof` and the field offsets
  (`native/include/pdftoolkit/memory/page_slab.hpp`). The deviation is
  documented at the `reserved` field itself.
* **Action for the audit owner:** amend task 1.3's snippet in
  `docs/issues.md` / issue #1 (`reserved[8]`).
* **Status:** `open` (audit text amendment outstanding; code side done
  and test-locked — including the acceptance criterion that coordinate
  offsets are 32-byte aligned).


## P-021 — Landed task-2.2 code half-parsed indirect `/Length` as a bogus byte count (code defect, fixed by 2.4)

* **Where:** `native/src/parser/xref.cpp`, the xref-stream `/Length`
  dictionary entry (task-2.2 code as landed in commit 59e6494).
* **What:** the `/Length` parser accepted any value starting with an
  integer — an indirect reference (`/Length 999 0 R`) was read as the
  direct byte count 999 (the reference's object number). Harmless for
  unfiltered rows (the extent was implicitly bounded by the file size
  and the row-count checks), but as a FLATE compressed extent it is
  actively wrong in both directions (a too-small object number truncates
  the payload; a too-large one feeds trailing `endstream` bytes into
  the codec).
* **Why it happened:** task 2.2 only needed `/Length` as a consistency
  bound; the chicken-and-egg note (indirect /Length unresolvable before
  the index exists) was documented but the half-parse was not.
* **What changed (2026-10-09, task 2.4):** `/Length` now goes through
  the same indirect-reference rejection as `/Prev`/`/XRefStm`
  (`parse_offset_value`); an indirect `/Length` leaves the section
  unparseable and the honest linear-scan degradation applies.
* **Verified:** `flate_indirect_length_degrades_to_linear_scan`
  (pdtk_test_xref) + the unfiltered corpus unaffected (all direct
  /Length values).
* **Status:** `fixed` (regression-locked).

## P-020 — XRef streams with predictor-bearing `/DecodeParms` degrade to the linear scan (deliberate scope line)

* **Where:** `native/src/parser/xref.cpp` (`/DecodeParms` handling).
* **What:** PDF 1.5+ xref streams may carry `/DecodeParms <<
  /Predictor 12 /Columns N >>` (PNG prediction) or `/Predictor 2`
  (TIFF). The resolver detects any predictor >= 2 (and any array-form
  `/DecodeParms`, i.e. a filter pipeline) and degrades the section to
  the emergency linear scan instead of decoding.
* **Why it is deliberate:** silently ignoring the predictor would
  mis-decode every row (wrong offsets, wrong types) — a CORRUPTED index
  that looks authoritative is far worse than a flagged partial index.
  The audit never mentions predictors in any task; there is no
  prescribed home for the decoder.
* **Resolution path:** a future task (natural home: Phase 3 ingestion,
  alongside stream consumers that also meet predictors on content
  streams) implements PNG/TIFF un-prediction between decompression and
  row decoding; the plumbing (decompressed rows in the scratch arena)
  already exists.
* **Status:** `open` (documented deferral; detection is test-locked by
  `flate_predictor_parms_degrade_to_linear_scan`).

## P-019 — Audit task-2.4 names "libdeflater" — no such C library exists (audit defect)

* **Where:** `docs/issues.md` (issue #1), Task 2.4.
* **What:** the audit prescribes integrating "`libdeflater` via CMake
  FetchContent". `github.com/ebiggers/libdeflater` does not resolve
  (verified 2026-10-09: git ls-remote fails); "libdeflater" is the name
  of a Rust *binding crate* for the C library **libdeflate**
  (ebiggers/libdeflate) — the canonical SIMD-accelerated Flate
  decoder the audit's description points at (hardware-accelerated,
  FetchContent-integrable CMake build, MIT-style license).
* **What was done:** integrated libdeflate **v1.26** (pinned release)
  under the `PDTK_ENABLE_FLATE` option — policy ADR-0007 (which also
  covers the offline-build consequence). Gate verified met:
  1034.7 MB/s mean on the level-6 content mix vs the audit's
  800 MB/s/core (recording:
  `docs/benchmarks/2026-10-09-phase2-flate-baseline.md`).
* **Action for the audit owner:** amend the task-2.4 library name in
  `docs/issues.md` / issue #1 to `libdeflate`.
* **Status:** `open` (audit text amendment outstanding; integration
  done, test-locked).

## P-022 — Reference sandbox carries a system libdeflate that masks missing third-party links (environment trap)

* **Where:** the reference sandbox (`libdeflate-dev` 1.23-2 installed:
  `/usr/include/libdeflate.h`); discovered by the task-2.4 CI run
  37925672301 (all three native jobs failed at Build on the clean
  ubuntu runner while the local matrix was green).
* **What:** `pdtk_test_xref` includes `libdeflate.h` (its zlib fixture
  helper) but was not linked to the `libdeflate_static` target.
  Locally the include silently resolved from `/usr/include` (system
  1.23 headers), and the symbols resolved from the FetchContent v1.26
  archive via CMake's `$<LINK_ONLY>` propagation through
  `pdftoolkit_core` — a version-mismatched header/library combination
  that happened to be ABI-compatible, so every local check passed. The
  clean CI runner has no system libdeflate and failed immediately.
* **Why it matters:** a green LOCAL matrix does not prove hermetic
  third-party isolation when the sandbox happens to carry the same
  library system-wide. The clean-runner CI job is the actual
  hermeticity check for this class of defect.
* **What changed (2026-10-09):** `pdtk_test_xref` links
  `libdeflate_static` under `PDTK_HAVE_FLATE` (pinned headers + same
  archive); its ninja INCLUDES line now leads with the `_deps` source
  dir. Rule for future work: any TU that includes a FetchContent'd
  third-party header must link that third-party's target explicitly —
  never rely on ambient system headers or transitive propagation.
* **Verified:** local re-verification after the fix (release 12/12,
  debug 12/12, asan 11/11, tsan 11/11, offline 12/12, Python 644) +
  the follow-up CI run on the clean runner.
* **Status:** `fixed` (regression-locked by CI itself; the trap is
  documented here for the next third-party integration).

## P-023 — The audit never assigns the object-graph resolver to any task (planning defect)

* **Where:** `docs/issues.md` (issue #1), Phase 3+ task texts.
* **What:** tasks 3.1/3.2/3.3 consume streams whose ACQUISITION path is
  specified nowhere: reaching a font's `/ToUnicode` program requires
  following `/Font` -> `/ToUnicode N 0 R` through indirect references,
  the page tree, and object streams (type-2 xref entries) — an
  object-graph resolver that no task in the audit's phase list builds.
  Task 2.2 produces the XRefIndex (where every object LIVES), and task
  3.1's registry entry already anticipated composing with it, but the
  reference-following layer itself (parse an indirect reference value,
  load a compressed object out of an /ObjStm) has no home. Same
  planning-defect class as P-012/P-017a: the dependency is real but
  unlisted.
* **Interim contract (implemented with task 3.1):**
  `CMapCache::intern_stream_object` takes the FULL stream-object bytes
  starting at the object header — given an XRefIndex entry, that is
  the object's file offset; the resolver-to-be only needs to hand over
  spans. When a future task builds the graph, this boundary is the
  seam it plugs into.
* **Resolution path:** a task (natural home: Phase 3 alongside 3.3's
  page ingestion, or a dedicated small task before it) implements the
  reference/value layer — indirect reference parsing, /ObjStm
  extraction (needs 2.4's codec, already landed), page-tree descent —
  and wires `CMapTable` lookups behind it.
* **Action for the audit owner:** add the missing task (or extend
  3.2/3.3's steps) in `docs/issues.md` / issue #1.
* **Status:** `open` (planning defect documented; consumer-side seam
  in place).

## P-024 — Ligature normalization covers Alphabetic Presentation Forms only; Arabic presentation forms and Unicode normalization are out of scope (deliberate scope line)

* **Where:** `native/src/font/cmap.cpp` (the verified ligature table).
* **What:** the audit's task-3.1 step 3 says "Normalize multi-character
  ligatures: Expand 0xFB01 to {'f', 'i'}" — one example, no boundary.
  The implemented boundary: every codepoint in U+FB00..U+FB4F whose
  Unicode name is a LIGATURE with a multi-character decomposition
  (14 entries: Latin ff/fi/fl/ffi/ffl/ſt/st, Armenian men-*/vew-now,
  Hebrew yiddish-double-yod-patah and alef-lamed), each verified
  against Python's `unicodedata`. Deliberately EXCLUDED: the Hebrew
  base+diacritic compositions in the same block (FB1D, FB2A..FB4E —
  accent compositions, not ligatures) and Arabic Presentation Forms
  (U+FB50+ — contextual joining forms whose expansion changes shaping
  semantics, and which text extractors do not expand).
* **Why it is deliberate:** expanding accent compositions would be
  Unicode NORMALIZATION (a different feature with different
  correctness rules — NFC/NFKC territory), and expanding Arabic
  contextual forms would corrupt text that shaping engines need
  intact. The audit asks for ligature repair, not normalization.
* **Resolution path:** if normalization is ever wanted, it is a
  separate, explicitly-scoped feature (probably at text-extraction
  time, not CMap-build time), with its own ADR.
* **Status:** `open` (documented scope line; table is test-locked by
  `ligature_table_is_complete`).
