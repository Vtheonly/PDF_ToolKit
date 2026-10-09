# FlateDecompressor throughput baseline (audit task 2.4) — 2026-10-09

## Command

```
cmake --preset release -DPython3_EXECUTABLE=$(which python)   # -O3, -march=native
cmake --build --preset release
scripts/run_perf.sh --json build/flate-reports \
    build/release/native/benchmarks/pdtk_bench_flate
# perf stage: SKIPPED (perf not installed; see U-010)
```

Environment: the reference sandbox (GCC 14.2.0, shared-vCPU container —
see the environment-ceiling caveat below). Binary:
`pdtk_bench_flate` (google/benchmark v1.9.5, 10 repetitions per
benchmark, per-repetition samples retained in
`build/flate-reports/pdtk_bench_flate.json`; percentiles by the
nearest-rank method documented in this folder's README). The codec is
libdeflate v1.26 (static, FetchContent — ADR-0007); the host CPU
reports AVX2 and AVX-512, and libdeflate's runtime dispatch selects the
SIMD inflate paths. The measured path is the production one:
`FlateDecompressor::decompress` staging output in doubling chunks
inside a `BumpArena`, `reset()` between iterations.

The headline metric is **decompressed output MB/s** (the
ingestion-relevant rate); each row's label carries the compressed size
and the realized compression ratio.

## Recorded results (10 repetitions)

| Benchmark | Mean | P50 / P90 / P99 | CV | Notes |
|---|---|---|---|---|
| `FlateDecompressor/control_traversal_16MiB` | **4.538 GB/s** | 4.505 / 4.655 / 4.664 GB/s | 5.0 % | byte-sum of the same 16 MiB buffer (U-012 control) |
| `FlateDecompressor/content_l1_16MiB` | **1036.0 MB/s** | 1035.9 / 1040.1 / 1043.6 MB/s | 1.3 % | content mix, compressed at level 1 (ratio 4.1x) |
| `FlateDecompressor/content_l6_16MiB` | **1034.7 MB/s** | 1035.0 / 1039.7 / 1040.7 MB/s | 1.6 % | content mix, level 6 (ratio 4.8x) — the common PDF producer default |
| `FlateDecompressor/content_l9_16MiB` | **1088.5 MB/s** | 1091.4 / 1099.8 / 1101.4 MB/s | 3.4 % | content mix, level 9 (ratio 5.0x) |
| `FlateDecompressor/repetitive_16MiB` | **4511.5 MB/s** | 4502.6 / 4544.9 / 4552.9 MB/s | 1.9 % | tiled rows, ratio 51.5x |
| `FlateDecompressor/incompressible_16MiB` | **7533.0 MB/s** | 7334.3 / 7807.2 / 7811.0 MB/s | 7.5 % | random bytes, ratio 1.00x (stored blocks — memcpy-class) |

Ratios to the same-run traversal control: content mix
**1035 / 4538 MB/s ≈ 23 %**; repetitive **4512 / 4538 ≈ 99 %**;
incompressible **7533 / 4538 ≈ 166 %** (inflate of stored blocks
streams faster than a scalar byte-sum of the same output, i.e. the
workload leaves the scalar traversal control behind).

## The audit's gate — met

Task 2.4's acceptance: "Decompression benchmark achieves >= 800 MB/s
per core on compressed streams." Measured on the representative PDF
content workload (the mix an ingest pipeline actually sees — text
operators, graphics state, paths, dicts, compressed at producer-default
level 6): **1034.7 MB/s mean (P99 1040.7)** of decompressed output,
single-threaded — **1.29x the gate**, or **23 % of the same-run
environment traversal ceiling**. The repetitive (rows/tables) and
incompressible (embedded binary) shapes are far above the gate. Unlike
task 2.3 (P-018), no blocked-on-later-phase technology is required:
this is the final throughput-gated Phase 2 component and it passes on
this sandbox as-is.

## Staging heuristic — the 8x first guess (measured, then fixed)

The initial benchmark run measured content throughput at 537–625 MB/s —
**below the gate** — and the cause was staging, not inflate: a first
guess of 4x the compressed size sits just below the ~4.8x realized
ratio of level-6 content, so EVERY decompression call paid one full
aborted attempt (fill 4x, rewind, redo at 8x) — ~45 % of the inflate
work discarded per call. Raising the first guess to 8x the compressed
size (`kGuessMultiplier`, `native/src/codec/flate.cpp`) single-shots
the common content ratios (3–6x) and lifted the measured rate to
1035–1088 MB/s, i.e. ~1.02–1.05x the underlying single-shot inflate
rate implied by the wasted-work arithmetic. Higher ratios amortize the
retry cost as ratio/(ratio+8) — 93 % efficiency at 100x. A second
benchmark-driven fix: when a speculative chunk exceeds the arena's
remaining capacity, the codec now retries with exactly that capacity
before refusing (the incompressible workload's 8x guess overshoots a
32 MiB arena while its 1.0x output fits — first observed as a skipped
benchmark, fixed in the same session).

## Environment-ceiling caveat (U-012)

Same-day control measurements: 4.49–4.99 GB/s across this session's
runs (and 4.59 GB/s in the lexer session earlier today) — the
shared-vCPU ceiling keeps moving run-to-run, which is why every ratio
above is computed against the SAME RUN's control. The durable
cross-environment statements are (a) the control ratios, and (b) the
gate margin (1.29x on this sandbox's content mix). On a
non-throttled core the absolute numbers should be strictly higher
(libdeflate documents 1–2 GB/s+ inflate on modern desktop CPUs).

## What IS established

* The audit's 800 MB/s/core gate is met on the representative workload
  with a 29 % margin, single-threaded, through the full production path
  (arena staging, no intermediate copies).
* Output staging in the caller's arena costs nothing measurable: the
  successful chunk is the output buffer itself (zero copies), and
  aborted attempts rewind rather than accumulate.
* The decompression-bomb defenses are off the measured path: content
  ratios (3–6x) sit far below the 128x cap, so no defense ever fires in
  the benchmark; their behavior is verified by the dedicated test suite
  (`pdtk_test_flate`, 19 cases).
