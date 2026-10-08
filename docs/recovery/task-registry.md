# Task Registry — Live Status

Single source of truth for the status of every tracked task in this
repository. **Rule: a task may only be marked `done` with a verification
command + captured result as evidence.** Statuses: `done`, `in_progress`,
`pending`, `blocked`, `wontfix` (with ADR), `not_applicable` (with proof).

Legend: **Task ID** `issue-1/task-X.Y` refers to `docs/issues.md` (= GitHub
issue #1). Pre-issues use the `T-NNN` prefix.

## Pre-issues (repository & agent infrastructure)

| ID | Task | Status | Evidence / Notes |
|----|------|--------|------------------|
| T-000 | Merge all branches into single `main`, delete others | `not_applicable` | Verified 2026-10-09 via `git ls-remote --heads origin` and GitHub API (`/branches`, `/pulls`): the repo contains **exactly one branch (`main`) and zero pull requests**. No merge or deletion was required; the desired state already held. |
| T-001 | Create `AGENTS.md` (first iteration) + `docs/agent/` guides | `done` | Commit `932cf07`. Written after full-repo inspection; environment facts verified live. |
| T-002 | Create continuity docs (`docs/recovery/*`, `docs/architecture/*`, `docs/decisions/*`, this registry) | `done` | This commit. Evidence: files exist, cross-referenced from AGENTS.md. |
| T-003 | Fix stale test-count claims (README/ARCHITECTURE say 646; suite has 644) | `done` | This commit. Evidence: `.venv/bin/python -m pytest --collect-only -q` → `644 tests collected`. Problem P-006. |
| T-004 | Fix `tests/adapters/test_http.py` collection error when `http` extra absent | `done` | Problem P-007. Reproduced 2026-10-09 (`ModuleNotFoundError` → collection abort). Fixed with `pytest.importorskip`. Verified: with extra → `644 passed`; without extra (clean `.[dev]` venv) → `589 passed, 5 skipped`, exit 0, no collection error. |

## Issue #1 — Native C++20 re-architecture epic (`docs/issues.md`)

### Phase 0 — Workspace scaffolding, tooling, modern C++20 infrastructure

| ID | Task | Status | Evidence / Notes |
|----|------|--------|------------------|
| issue-1/task-0.1 | Build system setup (CMake + Ninja, four modular targets) | `done` | Verified: fresh `cmake -B build -G Ninja && ninja -C build` (GCC 14.2, C++20, `-Wall -Wextra -Wpedantic -Wconversion -Werror`) → all four targets build with **zero warnings** (strict grep `warning:|error:` → 0). `ctest` → 6/6 passed. `nm -D` → FFI exports exactly the 6 `pdftoolkit_*` C symbols (pure C-ABI). CLI runs; Python extension imports. See ADR-0002, ADR-0003. |
| issue-1/task-0.2 | Automated benchmarking pipeline (Google Benchmark) | `done` | Pipeline: `PDTK_ENABLE_BENCHMARKS` option (default ON, ADR-0005, resolves U-006) + `FetchContent` google/benchmark **v1.9.5 pinned**; `native/benchmarks/` (bench exists only when its component exists — see P-012); `scripts/run_perf.sh` (single invocation per binary; perf stage degrades honestly). Verified 2026-10-09: zero-warning build; `scripts/run_perf.sh --json ...` → console+JSON report, perf stage `SKIPPED (perf not installed)` exit 0 (U-010 behaviour); offline path `-DPDTK_ENABLE_BENCHMARKS=OFF` → no `_deps`, 6/6 ctest; asan/tsan re-verified with benchmarks OFF (5/5 each, no FetchContent — preset change verified); debug 6/6; Python 644; ci.yml gains a benchmark step (**first live CI run green**, run 37826782644 on 9fb0bbc, 5/5 jobs incl. the benchmark step). Authoritative recording: `docs/benchmarks/2026-10-09-phase0-arena-baseline.md`. |
| issue-1/task-0.3 | Sanitizers and hardening pipeline (`CMakePresets.json`, CI) | `done` | Local (GCC 14.2): `release` 6/6, `debug` 6/6, `asan` (ASan+UBSan, `-fno-sanitize-recover=all`) 5/5 zero findings, `tsan` 5/5 zero findings, all with 0 compiler diagnostics. **First live CI run: SUCCESS** (run 37820960657 on a9c5e98 — all 5 job instances green: pytest py3.9, pytest py3.12, native release, native asan, native tsan). `msan` preset remains clang-only (P-008; GCC rejects it, verified). |

### Phase 1 — Native memory subsystem & virtual page slabs

| ID | Task | Status | Evidence / Notes |
|----|------|--------|------------------|
| issue-1/task-1.1 | Guarded memory-mapped buffer manager (`MmapHandle`) | `done` | `native/{include/pdftoolkit/memory/mmap.hpp, src/memory/mmap.cpp}` per audit spec; design ADR-0006 (audit's throw-from-handler is UB — P-014). Verified 2026-10-09: **truncation acceptance met** (`guard_converts_truncation_to_typed_exception`: ftruncate under a live mapping → `PdfToolkitException(IoTruncated)`, never a crash) + 14 further cases incl. three fork-based honesty deaths (no recovery point / foreign mapping / dead handle → default disposition), 128-slot capacity + recycling. Matrix: release zero-warning + ctest **7/7**, debug 7/7, asan+ubsan (no-recover) 6/6, tsan 6/6, offline (no FetchContent) 7/7, Python 644. FFI wiring: `pdftoolkit_register_document` now mmaps via `MmapHandle` + `advise(WillNeed)` (typed errors → C-ABI statuses; directories now correctly `UNREADABLE_PDF`). `pdtk_bench_mmap` landed (component-exists rule): warm traversal 2.55 GB/s mean = **~88–90 % of this environment's heap ceiling** (control experiment; recording `docs/benchmarks/2026-10-09-phase1-mmap-baseline.md`; U-012). Build change: `CMAKE_POSITION_INDEPENDENT_CODE ON` (TLS in a shared object — see ADR-0006). **Live CI green on 5346a2e** (run 37833962102: release+benchmarks, asan, tsan, pytest py3.9/3.12 — 5/5, mmap fork-based honesty tests clean under both sanitizers on the runner). |
| issue-1/task-1.2 | Thread-local bump-pointer arena allocator | `done` | `BumpArena` per audit spec + hardening (capacity rounding, non-copyable/movable, count-overflow guard); 11/11 unit tests green. **Authoritative measurement recorded** (delivered by task 0.2): 1M allocs + reset = mean 485.48 us, P50 482.15 / P90 496.27 / P99 513.31 us (20 reps, CV 2.4%); steady-state alloc+reset pair = **0.28 ns** (~0.9 cycles @3.2 GHz); ~2.06e9 allocs/s. Evidence: `docs/benchmarks/2026-10-09-phase0-arena-baseline.md`; regression guard < 10 ms in `test_arena.cpp` (Release-only). The audit-text correction (P-011) remains an open action for the audit owner. |
| issue-1/task-1.3 | Unified Page Slab (UPS) binary layout | `pending` | Depends on 1.1, 1.2. `static_assert(sizeof(PageSlabHeader) == 64)`. |

### Phase 2 — PDF binary protocol, object graph, SIMD decompression

| ID | Task | Status | Evidence / Notes |
|----|------|--------|------------------|
| issue-1/task-2.1 | Zero-copy backward `startxref` & trailer scanner | `pending` | Depends on 1.1. |
| issue-1/task-2.2 | Dual-mode XRef table & stream resolver | `pending` | Depends on 2.1, 1.2. |
| issue-1/task-2.3 | Zero-copy byte lexer & tokenizer | `pending` | Depends on 1.1. Throughput gate: > 2.5 GB/s. |
| issue-1/task-2.4 | Hardware-accelerated Flate decompressor | `pending` | Depends on 1.2. Gate: >= 800 MB/s/core. |

### Phase 3 — Fused operator stream & font CMap normalization

| ID | Task | Status | Evidence / Notes |
|----|------|--------|------------------|
| issue-1/task-3.1 | Immutable global CMap cache & Unicode resolver | `pending` | Depends on 2.3, 2.4. |
| issue-1/task-3.2 | Content stream operator state machine | `pending` | Depends on 3.1, 2.3. Precision gate: ±0.001 pt. |
| issue-1/task-3.3 | Single-pass slab population engine | `pending` | Depends on 1.3, 3.2. Zero heap allocations between stream input and completed slab. |

### Phase 4 — In-memory hybrid indexing & SIMD search

| ID | Task | Status | Evidence / Notes |
|----|------|--------|------------------|
| issue-1/task-4.1 | Corpus-level term lexicon & posting table | `pending` | Depends on 3.3. Gate: 10k pages < 1.0 s; < 1.5 B/token. |
| issue-1/task-4.2 | Block-Max WAND query engine | `pending` | Depends on 4.1. Gate: 15–40x over brute force. |
| issue-1/task-4.3 | AVX2/NEON direct needle vector search | `pending` | Depends on 1.1. Gate: > 4.0 GB/s. |
| issue-1/task-4.4 | SIMD spatial bounding-box filter | `pending` | Depends on 1.3. Gate: < 500 ns / 1k glyphs. |

### Phase 5 — Non-destructive structural operations

| ID | Task | Status | Evidence / Notes |
|----|------|--------|------------------|
| issue-1/task-5.1 | Virtual page graph compiler (`PagePlan`) | `pending` | Depends on 2.2. Gate: < 10 µs, zero allocations. |
| issue-1/task-5.2 | Zero-copy incremental PDF recompiler & writer | `pending` | Depends on 5.1, 1.1. Gate: 2×500-page merge < 15 ms. |

### Phase 6 — Multi-core concurrency & any-time scheduler

| ID | Task | Status | Evidence / Notes |
|----|------|--------|------------------|
| issue-1/task-6.1 | Lock-free atomic document registry | `pending` | Depends on 1.1. |
| issue-1/task-6.2 | Work-stealing execution pool | `pending` | Depends on 6.1. Gate: >= 14x on 16 threads. |
| issue-1/task-6.3 | Any-time execution scheduler (`SearchWatchdog`) | `pending` | Depends on 4.2, 6.2. Gate: 10 ms budget → partial results <= 10.5 ms. |

### Phase 7 — Zero-copy interfaces, C-FFI, bindings

| ID | Task | Status | Evidence / Notes |
|----|------|--------|------------------|
| issue-1/task-7.1 | Panic-proof C-ABI export layer | `pending` | Depends on Phases 1–6. FFI surface may scaffold earlier; implementations must not fake success. |
| issue-1/task-7.2 | Python Buffer-Protocol bindings | `pending` | Depends on 7.1. NumPy 2.2.4 available in env for tests. |
| issue-1/task-7.3 | Native headless CLI | `pending` | Depends on 7.1. Gate: < 5 ms invocation. |

### Phase 8 — Fuzzing, numerical verification, benchmarking

| ID | Task | Status | Evidence / Notes |
|----|------|--------|------------------|
| issue-1/task-8.1 | Continuous LLVM LibFuzzer harness | `pending` | Gate: 5×10^7 cycles, zero crashes. |
| issue-1/task-8.2 | Numerical stability & invariance suite | `pending` | Cross-arch determinism. |
| issue-1/task-8.3 | Competitive production benchmark publication | `pending` | Depends on all phases; scorecard in audit §4. |

## Next tasks (recommended order)

1. **issue-1/task-1.3** — Unified Page Slab layout (`static_assert` header +
   `PageSlabView` span accessors) + `pdtk_bench_slab`. Dependencies
   (1.1, 1.2) are now both done.
2. **issue-1/task-2.1** — zero-copy backward `startxref`/trailer scanner
   (depends only on 1.1, now unblocked) — or task-1.3 first if staying
   strictly inside Phase 1.
3. **MSan-with-clang CI job** (U-002 remainder) — pursue when Phase 1+
   code justifies the instrumented-libc++ setup cost.
4. **P-011/P-012/P-014 follow-ups** — audit owner corrects the
   mis-scaled task-1.2 bound, the task-0.2 dependency inversion and the
   task-1.1 throw-from-handler wording in `docs/issues.md` / issue #1.
5. **P-012 follow-up** — when landing bench_wand (task 4.2), satisfy the
   audit's original task-0.2 acceptance literally.

## Session log

| Date | Session | Tasks progressed |
|------|---------|------------------|
| 2026-10-09 | Session 2 (0.2 + 1.2 + 1.1) | Baseline re-verified green (Python 644, native 6/6). issue-1/task-0.2 done (pipeline + pdtk_bench_arena + run_perf.sh + CI step; ADR-0005; P-012; U-006 resolved; U-009/U-010 filed; CI green incl. benchmark step, run 37826782644 on 9fb0bbc). issue-1/task-1.2 closed with the authoritative measurement. **issue-1/task-1.1 done**: guarded `MmapHandle` + FFI mmap wiring + `pdtk_bench_mmap` (ADR-0006; P-013 false alarm withdrawn with methodology rule; P-014 filed; U-011/U-012 filed; ADR index gained the missing 0005 row). Verification matrix: release 7/7 zero-warning, debug 7/7, asan 6/6, tsan 6/6, offline 7/7, Python 644. |
| 2026-10-09 | Session 2 (0.2 + 1.2 closure) | Baseline re-verified green (Python 644, native 6/6). issue-1/task-0.2 done (pipeline + pdtk_bench_arena + run_perf.sh + CI step; ADR-0005; P-012; U-006 resolved; U-009/U-010 filed). issue-1/task-1.2 closed with the authoritative measurement. Verification matrix: offline 6/6, asan 5/5, tsan 5/5, debug 6/6, Python 644, YAML/JSON valid. |
| 2026-10-09 | Session 1 (issue-#1 kickoff) | T-000 verified N/A; T-001..T-004 done; baseline 644/644 green established; issue-1/task-0.1 done; issue-1/task-0.3 done (CI first run pending); issue-1/task-1.2 in progress (arena + 11 tests; benchmark pending 0.2); discoveries P-006..P-011, U-001..U-008 filed; ADR-0001..0004 recorded. |
