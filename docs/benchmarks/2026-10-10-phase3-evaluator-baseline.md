# Operator state machine baseline (audit task 3.2) — 2026-10-10

## Command

```
cmake --preset release -DPython3_EXECUTABLE=$(which python)   # -O3, -march=native
cmake --build --preset release
scripts/run_perf.sh --json build/evaluator-reports \
    build/release/native/benchmarks/pdtk_bench_evaluator
# perf stage: SKIPPED (perf not installed; see U-010)
```

Environment: the reference sandbox (GCC 14.2.0, shared-vCPU container —
see the environment-ceiling caveat below). Binary: `pdtk_bench_evaluator`
(google/benchmark v1.9.5, 10 repetitions per benchmark, per-repetition
samples retained in `build/evaluator-reports/pdtk_bench_evaluator.json`;
percentiles by the nearest-rank method documented in this folder's
README). The measured path is the production one: `OperatorEvaluator::
evaluate` over the task-2.3 ZeroCopyLexer with the default
FontMetricsResolver (constant w0 = 1000, 1-byte codes — the benchmark
measures the state machine, not a consumer's callback table) and a
counting sink.

Task 3.2's acceptance is a PRECISION gate (±0.001 pt vs the
synthetic-reference golden vectors, verified by pdtk_test_evaluator's
`golden_gate_within_0_001_pt` — 12 scenarios, 605 glyphs); there is no
throughput gate to pass or fail here. These numbers are the recorded
baseline task 3.3 builds on: its single-pass slab population is this
evaluator plus per-glyph slab writes.

## Workloads

* `text_heavy_16MiB` — BT/Tf/Td/T*/Tj blocks with long running-text
  strings (escape-free decode, advance math, 14-flop Trm, sink call
  per glyph; 12,292,247 glyphs per 16 MiB iteration).
* `kerned_tj_16MiB` — TJ arrays with a number between every pair of
  short strings (array byte-range record + re-lex + displacement
  adjustments; the Distiller-style layout mix; 2,856,416 glyphs per
  iteration — short strings amortize the per-showing-op cost poorly,
  which is exactly why the workload exists).
* `graphics_heavy_16MiB` — path/color/dash/marked-content noise with
  rare text (the tolerant skip path: operands accumulated and dropped,
  dicts and arrays skipped; 485,035 glyphs per iteration).
* `control_traversal_16MiB` — byte-sum of the same buffer (U-012
  methodology: the durable cross-environment number is the ratio
  evaluator/control measured in the SAME run).

## Results

| Workload | mean MB/s | P50 | P90 | P99 | CV | M glyphs/s (mean) | ns/glyph | % of same-run control |
|---|---|---|---|---|---|---|---|---|
| control_traversal | 4620.1 | 4617.8 | 4649.2 | 4650.5 | 0.51 % | — | — | 100 % |
| text_heavy | 204.8 | 204.5 | 205.8 | 206.1 | 0.44 % | 150.0 | 6.67 | 4.4 % |
| kerned_tj | 130.3 | 130.2 | 131.0 | 131.3 | 0.57 % | 22.2 | 45.0 | 2.8 % |
| graphics_heavy | 225.0 | 225.3 | 226.1 | 226.3 | 0.48 % | 6.5 | — | 4.9 % |

## Reading

* The evaluator is LEXER-BOUND, not state-machine-bound: every input
  byte passes through ZeroCopyLexer (whose own content-mix baseline was
  350.7 MB/s = 7.6 % of its control), and the state machine adds the
  per-glyph work on top. Text-heavy at 4.4 % of the same-run 4.62 GB/s
  traversal ceiling is consistent with that stack — the marginal cost
  of the state machine itself is the 6.67 ns/glyph of glyph-emission
  work, which the lexer-only baseline does not pay.
* The number task 3.3 should plan against is the GLYPH RATE for
  running text: **150 M glyphs/s** (6.67 ns/glyph) with a no-op sink.
  Kerned TJ pays 45 ns/glyph because every 2-glyph string carries a
  full showing-op preamble (P refresh, font-code-width resolve, TJ
  replay walk); pages dominated by such layouts will land between the
  two. The graphics-heavy skip path costs about the same per byte as
  text-heavy — tolerance is not the bottleneck.
* No optimization is scheduled off these numbers: no gate applies, and
  the P-018 lesson (SIMD classification in task 4.3 is what would move
  the lexer-bound ceiling) applies unchanged.

## Reproducibility

The workloads are LCG-generated (seeds 7/11/13 in
`native/benchmarks/bench_evaluator.cpp`); no corpus files are read.
Re-run the command above; JSON with all per-repetition samples lives
alongside this recording.
