# Change Log — Significant Discoveries & Completed Changes

Newest first. One entry per meaningful change or discovery, with commit
reference and evidence pointers. This is evolution history, not a changelog
for end users.

---

## 2026-10-09 — Session 2 (cont.): issue-1/task-1.1 done (guarded MmapHandle + FFI wiring + bench)

* **Guarded memory-mapped buffer manager landed (audit task 1.1):**
  `native/include/pdftoolkit/memory/mmap.hpp` +
  `native/src/memory/mmap.cpp`. POSIX `mmap(PROT_READ, MAP_PRIVATE)` +
  `posix_madvise` (WILLNEED/RANDOM), Win32 `CreateFileA`→`MapViewOfFile`
  compile-only path, immutable-span-only exposure, rule-of-five, typed
  constructor error mapping (InvalidArgument/DocumentNotFound/
  UnreadablePdf/Internal), 128-slot guard registry. **Acceptance
  criterion met and test-locked**: truncation under a live mapping →
  `PdfToolkitException(IoTruncated)` through `guarded()`, never a crash
  (`native/tests/test_mmap.cpp`, 15 cases incl. three fork-based honesty
  deaths and capacity/recycling).
* **Design deviation from the audit text (P-014, ADR-0006):** the audit
  prescribes *throwing from the sigaction handler* — undefined
  behaviour. Implemented compliantly instead: `sigsetjmp`/`siglongjmp`
  recovery point published per-thread; the handler converts only faults
  inside registered live mappings under an active recovery point;
  everything else is re-raised with the previously-installed disposition
  (sanitizer handlers chain through). Per-slot seqlock registry keeps
  the probe async-signal-safe, allocation-free and lock-free.
* **FFI wiring (the C-ABI's promised "arrives with task 1.1"):**
  `pdftoolkit_register_document` now mmaps the file via `MmapHandle` +
  `advise(WillNeed)` (sequential ingestion hint), keeping stable ids and
  the first mapping on re-registration. Directories are now correctly
  rejected as `PDTK_ERR_UNREADABLE_PDF` (previously accepted by the
  existence check); typed exceptions translate to C-ABI statuses.
  `pdftoolkit.h` scaffolding notes updated.
* **Build-system discovery (would break any TLS-using core code):**
  linking a non-PIC static core into the shared FFI library fails on
  TLS relocations (`R_X86_64_TPOFF32`). Fixed with
  `CMAKE_POSITION_INDEPENDENT_CODE ON` for the native tree.
* **`pdtk_bench_mmap` landed** (audit task 0.2's `bench_mmap.cpp` slot,
  component-exists rule): warm 128 MiB traversal mean 2.55 GB/s
  (P50 51.37 ms / P90 65.14 ms / P99 65.88 ms, CV 14.9 %);
  map+first-touch 1.94 GB/s (CV 4.2 %); open→advise→close lifecycle
  4.59 µs. Recording: `docs/benchmarks/2026-10-09-phase1-mmap-baseline.md`.
* **Environment-ceiling discovery (U-012):** a control experiment showed
  this sandbox traverses *any* 128 MiB buffer (heap, AVX-512-vectorized
  byte-sum) at only 2.40–2.85 GB/s — the durable cross-environment
  statement is "warm mmap retains ~88–90 % of heap traversal speed".
  The audit's future gates (lexer 2.5 GB/s, scanner 4.0 GB/s) sit at or
  above this environment's raw ceiling and must be reported as
  ratios when those tasks land.
* **CI false alarm (P-013, withdrawn):** the workflow's
  `branches: [main]` filter *displayed* as `branches: ain]` — the output
  rendering layer consumed `[m` as an ANSI escape. Diagnosed as a typo,
  then disproven byte-level (`od -c`) before commit: the file was always
  correct and CI triggered on every push as configured. Recorded as a
  verification-methodology pitfall (byte-dump before believing
  "corrupted output"). Also: first CI verification of the benchmark
  step came back green (run 37826782644 on 9fb0bbc, 5/5 jobs), closing
  task-0.2's "pending live CI run" note.
* **Docs hygiene:** ADR-0005 was missing from `docs/decisions/README.md`
  index — added, together with ADR-0006. U-011 (Windows SEH
  compile-only) and U-012 filed; native-tree.md updated to the new
  7-ctest reality.
* **Verification matrix (all 2026-10-09):** release zero-warning + ctest
  7/7; debug 7/7; asan+ubsan `-fno-sanitize-recover=all` 6/6; tsan 6/6;
  offline `-DPDTK_ENABLE_BENCHMARKS=OFF` 7/7 (no `_deps`); Python 644
  passed.
  **Live CI on the task-1.1 commit (5346a2e): all 5 jobs green** — run
  37833962102; the benchmark step now executes both `pdtk_bench_arena`
  and `pdtk_bench_mmap` on the runner.

---

## 2026-10-09 — Session 2: issue-1/task-0.2 done (benchmark pipeline); task-1.2 closed

* **Benchmarking pipeline landed (audit task 0.2):** `PDTK_ENABLE_BENCHMARKS`
  option (default ON) + FetchContent `google/benchmark` **v1.9.5 pinned**
  (`native/CMakeLists.txt`); `native/benchmarks/` with `pdtk_bench_arena`;
  `scripts/run_perf.sh` (discovery, single invocation per binary, perf-stat
  stage with honest degradation); CI native-release job now runs the suite
  on every push. Sanitizer presets build with benchmarks OFF. Policy:
  **ADR-0005** (resolves U-006).
* **Task-1.2 closed with the authoritative measurement:** 1M allocs+reset =
  mean 485.48 us, P50 482.15 / P90 496.27 / P99 513.31 us (20 reps, CV
  2.4%); alloc+reset pair = 0.28 ns. Recording:
  `docs/benchmarks/2026-10-09-phase0-arena-baseline.md` (P-011 updated).
* **Discovery P-012 (audit defect, same class as P-011):** task 0.2's
  acceptance names `bench_wand` whose engine arrives in Phase 4 (task 4.2);
  the audit's "depends on 0.1" line is wrong for three of its four named
  benches. Resolution: each bench lands with the task that builds its
  component; the map is recorded in `native/benchmarks/CMakeLists.txt`.
* **Pipeline discoveries (would cost a future agent real time):**
  * running a benchmark binary twice (console + JSON) yields two
    independent measurements that drifted 0.3-4% in the mean — the runner
    now invokes each binary exactly once (ADR-0005);
  * `ReportAggregatesOnly(true)` discards the per-repetition samples the
    P50/P90/P99 computation needs — forbidden for distribution-claiming
    benchmarks (percentile method + policy: `docs/benchmarks/README.md`);
  * google/benchmark v1.9.5 JSON tags per-repetition entries
    `"run_type": "iteration"` (not `"run"`), and has **no native percentile
    output** — nearest-rank percentiles are computed from the samples;
  * `perf` is not installed in the reference environment (U-010);
    `scripts/run_perf.sh` prints `SKIPPED (<reason>)` and exits 0 —
    a missing perf must never be misread as a benchmark failure.
* **GoogleTest deferral (U-009):** ADR-0003 predicted GoogleTest would land
  with task 0.2; scope-corrected by ADR-0005 — the harness survives until a
  test need exceeds it (expected: Phase 2 corpora / task 5.1 expected-error
  tests). The stale prediction in `native/CMakeLists.txt`'s test-section
  comment was fixed in the same commit.
* **Verification matrix (all 2026-10-09, GCC 14.2):** release zero-warning
  build; offline `-DPDTK_ENABLE_BENCHMARKS=OFF` → no `_deps`, ctest 6/6;
  asan 5/5 and tsan 5/5 (both with no FetchContent — preset change
  verified); debug 6/6 (benchmarks built at -O0, not run); Python 644
  passed; ci.yml YAML + CMakePresets.json validity checked.

---

## 2026-10-09 — Session 1, continued: issue-1/task-0.1 done (native build system)

* **Native C++20 build system landed (audit task 0.1):** root `CMakeLists.txt`
  shim + `native/` tree (ADR-0002) with the four audit targets:
  `pdftoolkit_core` (static), `pdftoolkit_ffi` (shared pure C-ABI),
  `pdftoolkit_py` (Python extension `pdftoolkit_native`, raw C-API per
  ADR-0003), `pdftoolkit_cli` (executable). Zero-warning policy enforced
  with `-Wall -Wextra -Wpedantic -Wconversion -Werror` (GCC/Clang) and
  `/W4 /permissive- /WX` (MSVC).
* **Verification evidence:** fresh configure+build → 0 warnings
  (strict `warning:|error:` grep on full build log); `ctest` 6/6 passed
  (arena, errors, ffi, c_abi_smoke, cli_version, python_import_smoke);
  `nm -D` shows the FFI library exports **exactly** the 6 `pdftoolkit_*`
  C symbols (visibility hygiene: hidden-by-default + `PDTK_API`
  annotations — an initially leaked mangled C++ symbol was found and
  eliminated); `pdftoolkit-cli --version` → `pdftoolkit-cli 0.1.0 (native
  core)`; Python import + all three extension functions work.
* **C-ABI contract established early (task 7.1 shape):** panic-proof
  (`PDTK_NOEXCEPT` + try/catch → status codes), honest stubs
  (`search_wand` → `PDTK_ERR_NOT_IMPLEMENTED`), stable doc ids for
  `register_document` (existence-validation only until task 1.1).
* **BumpArena implemented (task 1.2, partial → `in_progress`):** audit
  spec + three hardening fixes (capacity rounding for `aligned_alloc`'s
  C11 multiple-of-alignment rule; non-copyable/movable to kill the
  sketch's double-free; `count * sizeof(T)` overflow guard). 11/11 unit
  tests pass; 1M allocations + reset measured at 423,258 ns.
* **Discovery P-011:** the audit's task-1.2 acceptance bound
  (< 30 µs for 1,000,000 allocations) is physically impossible
  (30 ps/allocation); recorded with measurement and a correction request
  for the audit text. Code regression guard set at < 10 ms (intent-level).
* **Discovery U-008:** MSVC `_aligned_malloc` path is compile-only until
  a Windows CI run exists.
* **Environment note persisted:** cmake/ninja live in `.venv` (pip);
  configure needs `PATH=$PWD/.venv/bin:$PATH` — recorded in
  AGENTS.md §7 and the workflow docs.

---

## 2026-10-09 — Session 1, continued: P-007 fixed (T-004)

* **`tests/adapters/test_http.py` collection bug fixed:** replaced the
  ineffective `pytestmark = skipif(...)` + module-level fastapi import
  (which aborts the entire suite at collection when the `http` extra is
  missing) with `pytest.importorskip("fastapi")` / `("httpx")` — the same
  pattern its sibling `test_http_upload.py` already used.
* **Verification (both directions):**
  * with `http` extra: `644 passed, 1 warning` — no regression;
  * clean `.[dev]`-only venv: `589 passed, 5 skipped`, exit code 0,
    `SKIPPED` reasons all read `could not import 'fastapi'` — the
    documented "skip cleanly" behaviour now actually holds.
* **Accounting note (so the numbers never confuse anyone again):** the two
  HTTP modules hold 28 + 24 tests; a module-level skip counts as **one**
  skip entry, and 3 http-specific tests in `test_edge_cases.py` skip at
  function level → 644 − 55 = 589 passed + 5 skips.

---

## 2026-10-09 — Session 1 (issue #1 kickoff: agent infrastructure + continuity system)

* **Repository state verified (T-000, `not_applicable`):** the repo has
  exactly one branch — `main` — and zero pull requests
  (`git ls-remote --heads origin`; GitHub API `/branches`, `/pulls`).
  The requested "merge all branches into main and delete the others" end
  state already held; no merge or deletion was performed, so no history
  was lost or rewritten.
* **AGENTS.md created (T-001, commit `932cf07`)** as the first iteration
  of permanent agent instructions, plus `docs/agent/` guidance folder
  (index, workflows, DOs/DON'Ts).
* **Continuity documentation system created (T-002, this commit):**
  `docs/recovery/` (task registry, problem registry, unknowns, this change
  log), `docs/architecture/` (python-engine map, native-tree plan),
  `docs/decisions/` (ADR-0001..0004).
* **Environment verified and pinned in docs:** GCC 14.2.0 (C++20-capable),
  CMake 4.4.4 + Ninja 1.13.2 via pip venv (not system-installed), Python
  3.12.14, PyMuPDF 1.28.2, pytest 9.1.1, NumPy 2.2.4; system Python is
  PEP-668-managed (`.venv/` mandatory). Full extras installed:
  `pip install -e ".[dev,http]"`.
* **Baseline established:** `644 passed, 1 warning in 5.53s`
  (`.venv/bin/python -m pytest`) and `644 tests collected` — the Python
  engine is green at the session start SHA `297819d`.
* **Discovery P-006:** README/ARCHITECTURE claimed 646 tests; actual 644.
  Counts corrected (T-003) with the measuring command recorded.
* **Discovery P-007:** `tests/adapters/test_http.py` errors at collection
  (instead of skipping) when the `http` extra is absent — the module-level
  `fastapi` import defeats its own `skipif` guard. Reproduced live; fix
  filed as T-004 (pending).
* **Discovery P-003-constraint / ADR-0004:** the tempting Python-level
  "single-pass combined-keyword regex" optimization of `count_keywords`
  is semantically impossible (leftmost-first alternation and lookahead
  scanning both break documented count semantics). Documented so no future
  agent burns time on it.
* **ADRs recorded:** 0001 single-branch policy; 0002 native-tree isolation
  under `native/`; 0003 Phase-0 offline-build minimal-dependency rule;
  0004 rejection of the Python-level scan optimization.

---

## 2026-10-09 — Session 1, continued: issue-1/task-0.3 done (sanitizer + CI pipeline)

* **`CMakePresets.json` landed:** `release`, `debug`, `asan`
  (Address+UBSan with `-fno-sanitize-recover=all` — any finding fails the
  test), `tsan`, and `msan` (CI-only; clang + instrumented libc++
  required), each with matching build/test presets and isolated
  `build/<preset>` directories.
* **Sanitizer test exclusion policy:** the `python_import_smoke` ctest is
  excluded in sanitizer presets — loading an instrumented extension into
  an uninstrumented CPython is a toolchain-unsupported, meaningless
  configuration. Rationale documented in the preset description.
* **Verified locally (GCC 14.2):** release 6/6, debug 6/6, asan 5/5 with
  **zero sanitizer findings**, tsan 5/5 with zero findings; all builds
  had 0 compiler diagnostics. The `msan` preset fails with GCC exactly as
  P-008 documents (`cc: error: unrecognized argument to '-fsanitize='
  option: 'memory'`).
* **`.github/workflows/ci.yml` created:** push-triggered (single-branch
  policy), 4 jobs — Python suite on a 3.9/3.12 matrix, native release
  (zero-warning gate), native asan, native tsan. Includes the
  `vm.mmap_rnd_bits=28` mitigation for the known ubuntu-24.04
  sanitizer/ASLR shadow-memory runner issue. YAML-validated locally;
  **first live run pending** (tracked as U-002).
* **Discovery — timing guards are configuration-sensitive:** the arena's
  1M-allocation wall-clock regression guard tripped under ASan
  (41.6 ms vs 0.42 ms) and under Debug/-O0 (20.6 ms). Fixed by gating the
  assertion on optimized, uninstrumented builds only
  (`PDTK_TIMING_GUARDS_OFF` in `native/tests/test_arena.cpp`, covering
  `__has_feature`/`__SANITIZE_*` and `NDEBUG`). Rule added to
  dos-and-donts.md: **functional assertions always; wall-clock assertions
  only in Release without instrumentation**.

---

## 2026-10-09 — Session 1, close-out: first live CI run GREEN

* **CI confirmed on first live run** (run 37820960657, commit a9c5e98):
  all 5 job instances passed — `Python engine (pytest, py3.9)`,
  `py3.12`, `Native core (release, zero warnings)`,
  `Native core (AddressSanitizer + UBSan)`, `Native core (ThreadSanitizer)`.
  Notably this also re-verified the Python engine on **Python 3.9** (the
  oldest supported version) for the first time, and the native suite on
  GitHub's GCC — independent toolchain confirmation of the local results.
* U-002 resolved for the GCC/runner part; MSan-with-clang job remains a
  deliberate follow-up (needs instrumented libc++).
