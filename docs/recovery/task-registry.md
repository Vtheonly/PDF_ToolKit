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
| T-004 | Fix `tests/adapters/test_http.py` collection error when `http` extra absent | `pending` | Problem P-007. Reproduced 2026-10-09 (`ModuleNotFoundError: No module named 'fastapi'` → `1 error during collection`, suite aborted). Fix: `pytest.importorskip` pattern as already used by `test_http_upload.py`. |

## Issue #1 — Native C++20 re-architecture epic (`docs/issues.md`)

### Phase 0 — Workspace scaffolding, tooling, modern C++20 infrastructure

| ID | Task | Status | Evidence / Notes |
|----|------|--------|------------------|
| issue-1/task-0.1 | Build system setup (CMake + Ninja, four modular targets) | `pending` | Acceptance: `cmake -B build -G Ninja && ninja -C build` builds `pdftoolkit_core`, `pdftoolkit_ffi`, `pdftoolkit_py`, `pdftoolkit_cli` with zero warnings. See ADR-0002, ADR-0003. |
| issue-1/task-0.2 | Automated benchmarking pipeline (Google Benchmark) | `pending` | Requires FetchContent network access at configure time — see ADR-0003 for the deferral rationale. |
| issue-1/task-0.3 | Sanitizers and hardening pipeline (`CMakePresets.json`, CI) | `pending` | ASan/UBSan/TSan verifiable locally with GCC 14.2; MSan requires clang → CI-only (unknown U-004). |

### Phase 1 — Native memory subsystem & virtual page slabs

| ID | Task | Status | Evidence / Notes |
|----|------|--------|------------------|
| issue-1/task-1.1 | Guarded memory-mapped buffer manager (`MmapHandle`) | `pending` | Depends on 0.1. |
| issue-1/task-1.2 | Thread-local bump-pointer arena allocator | `pending` | Depends on 0.1. Spec fully given in the audit. |
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

1. **T-004** — one-line test-suite robustness fix, unblocks clean `.[dev]`-only runs.
2. **issue-1/task-0.1** — CMake + four targets, zero-warning build (unblocks all native phases).
3. **issue-1/task-0.3** — sanitizer presets + CI (parallel-safe after 0.1).
4. **issue-1/task-0.2** — Google Benchmark pipeline (needs network-at-configure policy decision, ADR-0003).
5. **issue-1/task-1.1 / 1.2 / 1.3** — memory subsystem (audit spec is fully prescriptive).

## Session log

| Date | Session | Tasks progressed |
|------|---------|------------------|
| 2026-10-09 | Session 1 (issue-#1 kickoff) | T-000 verified N/A; T-001, T-002, T-003 done; P-006/P-007 filed; baseline 644/644 green established. |
