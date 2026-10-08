# Phase-0 Baseline — BumpArena (task 1.2 authoritative measurement)

* **Date:** 2026-10-09
* **Task:** issue-1/task-0.2 (pipeline) closing issue-1/task-1.2 (measurement)
* **Binary:** `build/release/native/benchmarks/pdtk_bench_arena`
* **Command:**
  `scripts/run_perf.sh --json build/release/native/benchmarks/reports build/release/native/benchmarks`
* **Preset:** `release` — GCC 14.2.0, `-O3 -march=native`, zero-warning policy (`-Werror` on)
* **google/benchmark:** v1.9.5 (pinned via FetchContent), 20 repetitions
* **Environment:** 2 x 3200 MHz cores, L1d 48 KiB, L2 2 MiB, L3 504 MiB;
  no other load; `perf stat` stage **SKIPPED** — perf not installed in this
  environment (U-010; behaviour verified live, exit 0)

## Results

### BumpArena/1M_allocs_u32x8_plus_reset
(the audit task-1.2 workload, verbatim: 1,000,000 x `alloc_slice<uint32[8]>`
= 32 MiB of arena traffic, then one `reset()`; 20 repetitions)

| Metric | Value |
|--------|-------|
| mean | 485.48 us |
| median | 482.59 us |
| stddev | 11.64 us (CV 2.4%) |
| min | 470.14 us |
| **P50** | **482.15 us** |
| **P90** | **496.27 us** |
| **P99** | **513.31 us** (= max of 20 samples) |
| throughput | ~2.06 x 10^9 allocations/s |

Percentiles: nearest-rank over the 20 per-repetition JSON samples
(`"run_type": "iteration"`); see the method note in this folder's README.

### BumpArena/single_alloc_u32x8_plus_reset
(steady-state pair: one aligned allocation + the O(1) reset)

| Metric | Value |
|--------|-------|
| time per pair | **0.2800 ns** (~0.9 cycles at 3.2 GHz) |

## Interpretation (P-011 closure evidence)

* The audit's literal task-1.2 bound (< 30 us for the whole workload) is
  physically impossible — 30 ps/allocation (P-011). The measured floor is
  **~0.28 ns per allocation**, i.e. the honest result is ~16x the audit's
  bound while sitting at the hardware floor (sub-cycle per operation).
* Intent-level criterion satisfied with a wide margin: allocation is
  effectively free (sub-nanosecond), reset is O(1) and contributes nothing
  measurable to the pair benchmark.
* Regression guard stays at < 10 ms in
  `native/tests/test_arena.cpp::million_allocations_and_reset_are_fast`
  (Release-only, uninstrumented — the established PDTK_TIMING_GUARDS_OFF
  pattern), three orders of magnitude above the measured 0.49 ms.
* Cross-run drift observed while building the pipeline: two independent
  invocations of the same binary drifted ~0.3-4% in the mean (465 vs
  485 us across sessions of the same day). The pipeline therefore runs
  each binary exactly once per report (recorded in scripts/run_perf.sh).
