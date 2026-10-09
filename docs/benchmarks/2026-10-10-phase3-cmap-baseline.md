# CMap cache & Unicode resolver baseline (audit task 3.1) — 2026-10-10

## Command

```
cmake --preset release -DPython3_EXECUTABLE=$(which python)   # -O3, -march=native
cmake --build --preset release
scripts/run_perf.sh --json build/cmap-reports \
    build/release/native/benchmarks/pdtk_bench_cmap
# perf stage: SKIPPED (perf not installed; see U-010)
```

Environment: the reference sandbox (GCC 14.2.0, shared-vCPU container —
see the environment-ceiling caveat below). Binary: `pdtk_bench_cmap`
(google/benchmark v1.9.5, 10 repetitions per benchmark, per-repetition
samples retained in `build/cmap-reports/pdtk_bench_cmap.json`;
percentiles by the nearest-rank method documented in this folder's
README). The measured paths are the production ones:
`CMapTable::parse` over the ZeroCopyLexer, `CMapTable::lookup` on
frozen tables, and `CMapCache::intern_stream_object` miss/hit.

Task 3.1's acceptance is ACCURACY ("accurate decoding of complex
CID-keyed and TrueType subset fonts"), not a throughput gate — these
numbers are the recorded baseline for task 3.3, whose per-glyph hot
path is `CMapTable::lookup`.

## Workloads

* `parse_cid/2000` — a realistic 2-byte CID-keyed /ToUnicode program
  (contiguous bfranges + an array-form range + bfchar tail; 2,000
  mapped codes, 2,745 bytes of CMap text) parsed from raw text.
* `parse_ttf/200` — a 1-byte TrueType-subset program (sparse bfchar;
  127 codes).
* `lookup<2>/2000`, `lookup<1>/200` — O(1) direct-index lookups over a
  random code mix (2-byte: 2 % hit rate; 1-byte: 48 %), the exact
  operation task 3.3 performs per glyph.
* `intern_miss` — a DISTINCT-payload stream-object intern (anatomy +
  xxHash64 + parse + table build; raw payload, 512 codes).
* `intern_hit` — a repeat-payload intern (xxHash64 + exact byte
  compare only).
* `control` — byte-sum of the same buffer (the U-012 same-run ceiling;
  this run: **5.455 GB/s**).

## Recorded results (10 repetitions)

| Benchmark | Mean | P50 / P90 / P99 | CV | Derived |
|---|---|---|---|---|
| `bench_parse_cid/2000` | 51.36 us | 51.34 / 51.47 / 51.54 us | 0.2 % | **53.4 MB/s** CMap text (1.0 % of control) |
| `bench_parse_ttf/200` | 10.86 us | 10.73 / 11.08 / 11.64 us | 2.6 % | **233.2 MB/s** CMap text (4.3 % of control) |
| `bench_lookup<2>/2000` | 5.85 us | 5.82 / 5.90 / 6.07 us | 1.3 % | **700.6 M lookups/s** (1.43 ns/lookup) |
| `bench_lookup<1>/200` | 7.33 us | 7.27 / 7.43 / 7.80 us | 2.3 % | **559.2 M lookups/s** (1.79 ns/lookup) |
| `bench_intern_miss` | 16.84 us | 16.80 / 16.92 / 16.96 us | 0.4 % | 59.4 k distinct-payload interns/s |
| `bench_intern_hit/2000` | 1.07 us | 1.06 / 1.07 / 1.07 us | 0.4 % | **938.6 k repeat interns/s** |
| `bench_control` | 0.503 us | 0.503 / 0.505 / 0.505 us | 0.3 % | 5.455 GB/s traversal (U-012) |

## Reading the numbers

* **The hot path is where the design spent its complexity budget.**
  `lookup` is one bounds check + one dependent load into a frozen
  pool: 1.4-1.8 ns/lookup regardless of table width, ~4-5 cycles at
  this sandbox's effective clock. Task 3.3's per-glyph CMap cost is
  therefore noise next to its coordinate math.
* **Parse is a build-time cost, paid once per DISTINCT CMap** (the
  cache's dedup key is the payload bytes; repeat interns cost 1.07 us
  — 16x cheaper than the miss). The CID program parses at 53 MB/s
  because every code in every bfrange materializes a record + pool
  entry (2,000 emits for 2.7 KB of text — parse is code-density
  bound, not byte-density bound; the TTF program, whose bfchar lines
  are the byte-dense form, parses 4.4x faster). A 60 KB worst-case
  real-world ToUnicode parses in ~1.1 ms, once.
* **No Flate scenario** is measured here: decompression throughput is
  `pdtk_bench_flate`'s subject (2026-10-09 baseline: 1034.7 MB/s
  content mix); duplicating it would drag libdeflate includes into
  another TU (P-022 rule) for no new signal.
* The parse-internals note: the first measurement round ran at
  42.9 MB/s because every hex string allocated a fresh digit vector;
  the parse path now shares one scratch buffer (allocation-free
  modulo record/pool growth), worth ~25 % on CID and ~2.7x on TTF.

## Environment caveat (U-012)

The absolute numbers are environment-bound (shared-vCPU container;
the same-run control measured 5.455 GB/s — the ceiling itself moved
1.7x between runs on prior days). Cross-environment comparisons should
use the ratios to the control reported above, per the U-012 rule.
