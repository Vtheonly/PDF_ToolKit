# ZeroCopyLexer throughput baseline (audit task 2.3) — 2026-10-09

## Command

```
cmake --preset release -DPython3_EXECUTABLE=$(which python)   # -O3, -march=native
cmake --build --preset release
scripts/run_perf.sh --json build/lexer-reports \
    build/release/native/benchmarks/pdtk_bench_lexer
# perf stage: SKIPPED (perf not installed; see U-010)
```

Environment: the reference sandbox (GCC 14.2.0, shared-vCPU container —
see the environment-ceiling caveat below). Binary:
`pdtk_bench_lexer` (google/benchmark v1.9.5, 10 repetitions per
benchmark, per-repetition samples retained in
`build/lexer-reports/pdtk_bench_lexer.json`; percentiles by the
nearest-rank method documented in this folder's README).

## Recorded results (10 repetitions)

| Benchmark | Mean | P50 / P90 / P99 | CV | Notes |
|---|---|---|---|---|
| `ZeroCopyLexer/control_traversal_16MiB` | **4.586 GB/s** | 4.536 / 4.762 / 4.784 GB/s | 2.2 % | byte-sum of the same 16 MiB buffer (U-012 control) |
| `ZeroCopyLexer/content_stream_16MiB` | **350.7 MB/s** | 350.8 / 352.6 / 353.2 MB/s | 0.6 % | realistic operator mix; **67.71 M tokens/s** |
| `ZeroCopyLexer/string_heavy_16MiB` | **833.3 MB/s** | 833.5 / 837.8 / 839.5 MB/s | 0.5 % | escapes + balanced parens; 58.70 M tokens/s |

Ratios to the same-run traversal control: content mix
**351 / 4586 MB/s ≈ 7.6 %**; string-heavy **833 / 4586 ≈ 18.2 %**.

## The audit's gate — measured, and honestly unmet

Task 2.3's acceptance: "Tokenization throughput exceeds 2.5 GB/s on
uncompressed streams." Measured: **350.7 MB/s** on the content mix —
~14 % of the gate. This is not an environment artifact alone (today's
control traversal measured 4.59 GB/s, well ABOVE the gate): the gate
exceeds what a scalar, token-at-a-time lexer can deliver at all. At the
measured token density (~5 bytes/token, 67.7 M tokens/s), each token
costs ~14.7 ns ≈ 45 cycles — consistent with per-byte table dispatch
plus `from_chars` number parsing, i.e. the design the audit itself
prescribes for 2.3 (branchless LUT, token stream). Reaching 2.5 GB/s at
this token density requires ~500 M tokens/s ≈ 2 ns/token ≈ 6
cycles/token — SIMD-classification territory (classify 32 bytes per
instruction), which the audit introduces in **task 4.3** (AVX2/NEON
vector search). The gate is therefore recorded as **unmet, blocked on
the audit's own Phase 4 technology** (P-018).

## Environment-ceiling caveat (U-012 update)

The phase-1 mmap baseline (recorded earlier the same day) measured this
sandbox's heap traversal ceiling at 2.40–2.85 GB/s; today's control
measured **4.50–4.78 GB/s** — the shared-vCPU container's ceiling moved
~1.7× between runs. Any absolute number recorded here inherits that
variance; the durable statements are (a) the control ratio above, and
(b) the per-token cycle cost. Cross-environment comparisons must cite
the ratio, not the absolute throughput.

## What IS established

* The zero-copy contract costs nothing measurable per token: values are
  `string_view`s into the input (test-locked), and 59–68 M tokens/s is
  the same order as production scalar lexers on comparable hardware.
* The string scanner is 2.4× faster than the operator mix — literal
  strings are single-pass runs; numbers pay the `from_chars` cost.
* Determinism and memory safety are test-locked (200 hostile random
  buffers re-lexed identically; the suite runs clean under ASan+UBSan
  and TSan — see the task-2.3 registry entry).

## Follow-ups

* P-018: the audit owner re-scopes the 2.3 gate (tokens/s, or defer the
  GB/s figure to the task-4.3 SIMD rewrite of the token stream).
* If Phase 3 ingestion makes the lexer hot before 4.3 lands, the first
  scalar optimization target is the number path (fuse the real-detection
  scan into `from_chars`'s walk); the string path needs nothing.
