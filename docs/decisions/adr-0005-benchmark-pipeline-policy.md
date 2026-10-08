# ADR-0005 — Benchmark pipeline policy: FetchContent behind an option, benchmarks off in sanitizers

* **Status:** accepted (2026-10-09)
* **Supersedes:** none. **Amends:** ADR-0003 consequence 3 (GoogleTest
  landing "with task 0.2") and its offline-build scope; resolves U-006.
* **Context:** Audit task 0.2 prescribes `google/benchmark` via CMake
  `FetchContent` plus a `perf stat` automation script. ADR-0003 committed
  Phase 0 to offline builds (no FetchContent), deferred the network policy
  to "when task 0.2 is implemented" (U-006), and predicted the in-repo test
  harness would be replaced by GoogleTest in task 0.2.

* **Decision:**
  1. **`PDTK_ENABLE_BENCHMARKS` CMake option, default ON.** ON = configure
     fetches `google/benchmark` (pinned release tag — v1.9.5 today; bumps
     are deliberate, never a floating branch). OFF = the benchmark suite is
     skipped entirely, no network is touched, and the four engine targets
     build exactly as before — the ADR-0003 offline guarantee is preserved
     by construction (`cmake -B build -DPDTK_ENABLE_BENCHMARKS=OFF`).
     Developer machines and CI runners have network; air-gapped machines
     pass the OFF flag. This resolves U-006.
  2. **Sanitizer presets disable benchmarks** (`PDTK_ENABLE_BENCHMARKS=OFF`
     in the asan/tsan/msan presets): sanitizer runs stay lean (no
     FetchContent, no instrumented third-party build) and measure the test
     suite, not the benchmark harness.
  3. **One invocation per binary per report.** Running a binary once for
     console and once for JSON produced two independent measurements that
     drifted ~0.3-4% in the mean; `scripts/run_perf.sh` runs each binary
     exactly once (JSON mode when `--json` is given, console mode otherwise).
  4. **Percentile method** (google/benchmark v1.9.5 has no native P50/P90/
     P99 output): compute nearest-rank percentiles from the per-repetition
     JSON samples; benchmarks that claim a distribution register
     `Repetitions(N>=20)` and never `ReportAggregatesOnly` (it discards
     the samples). Policy lives in `docs/benchmarks/README.md`.
  5. **GoogleTest does NOT land with task 0.2** (scope correction, see
     below). The minimal in-repo harness (ADR-0003) survives until a test
     need exceeds it — expected trigger: Phase 2 parser corpora or
     expected-error-path tests; tracked as U-009.

* **Alternatives considered:**
  * *Vendoring google/benchmark into the repo* — rejected (ADR-0003
    already rejected it; ~1 MB of third-party code and an update liability).
  * *Default OFF for benchmarks* — rejected: the audit's whole point for
    0.2 is automated benchmarking on every push; default OFF would make CI
    silently skip it. Offline users are the exception and get a one-flag
    escape hatch instead.
  * *Benchmarks in every preset incl. sanitizers* — rejected: doubles
    sanitizer CI time for numbers nobody cites (sanitizer timing is
    meaningless anyway — the PDTK_TIMING_GUARDS_OFF discovery).
  * *Migrating native tests to GoogleTest in 0.2* — rejected on scope:
    the audit's task-0.2 text is about benchmarking; a test-framework
    migration is a separate change with its own verification cost. It was
    a prediction in ADR-0003, not a requirement of the audit; this ADR
    corrects the prediction's timing (U-009 tracks the real trigger).

* **Consequences:**
  * First configure with benchmarks ON requires network (documented);
    every later incremental build does not (FetchContent is cached in
    `build/<preset>/_deps`).
  * CI's native-release job runs the benchmark suite on every push and
    fails if a benchmark binary fails; `perf stat` is expected to be
    SKIPPED on hosted runners (U-010) and must never be treated as a
    benchmark failure.
  * Performance claims in any document must cite a recording under
    `docs/benchmarks/` (enforced by AGENTS.md section 5.7).
