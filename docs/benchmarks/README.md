# Benchmark Recordings — Policy and Ledger

Home of the authoritative performance numbers for the native core
(audit issue-1/task-0.2). Rule: **a performance number that is not recorded
here, with its command and environment, does not exist** — it may not be
cited in docs, registries, commit messages or the audit scorecard
(AGENTS.md section 5.7).

## How to run

```bash
cmake --preset release -DPython3_EXECUTABLE=$(which python)  # benchmarks ON by default
cmake --build --preset release
scripts/run_perf.sh                       # discover + run every built benchmark
scripts/run_perf.sh --json <dir> <bin>..  # machine-readable reports (one run per binary)
cmake -B build -DPDTK_ENABLE_BENCHMARKS=OFF ...   # offline builds skip the suite (ADR-0005)
```

## What every recording must contain

One file per recorded run, named `YYYY-MM-DD-<scope>-baseline.md`:

1. **Command** — the exact `scripts/run_perf.sh` invocation and preset.
2. **Environment** — CPU, cores, compiler, flags, google/benchmark version,
   and any degraded stage (e.g. perf SKIPPED, U-010).
3. **Numbers** — every reported metric with its unit, plus the aggregate
   table (mean / median / stddev / CV) and the distribution percentiles.
4. **Method notes** — anything a future reader needs to reproduce or
   correctly interpret the numbers (see the P50/P90/P99 note below).

## P50/P90/P99 method (pipeline limitation, recorded deliberately)

google/benchmark **v1.9.5 has no native percentile output** (its JSON
aggregates are mean/median/stddev/cv only). Percentiles are therefore
computed from the per-repetition `"run_type": "iteration"` samples in the
JSON report (nearest-rank). Consequences:

* benchmarks that claim a distribution must register `->Repetitions(N)`
  (N >= 20 recommended) and must **not** use `ReportAggregatesOnly` —
  that flag discards the per-repetition samples the percentiles need
  (discovered live during task 0.2; do not reintroduce it);
* if a future google/benchmark release adds native percentiles, bump the
  pinned `GIT_TAG` in `native/CMakeLists.txt` and fold this note away.

## Which components have benchmarks

A benchmark exists exactly when the component it measures exists —
benchmarks never fake numbers for unbuilt engines (P-012). Current map
lives at the top of `native/benchmarks/CMakeLists.txt`.

## Recorded runs

| Date | File | Scope |
|------|------|-------|
| 2026-10-09 | [2026-10-09-phase0-arena-baseline.md](2026-10-09-phase0-arena-baseline.md) | BumpArena (task 1.2 authoritative measurement) |
