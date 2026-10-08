# ADR-0003 — Phase 0 builds offline with zero external C++ dependencies

* **Status:** accepted (2026-10-09)
* **Supersedes:** none
* **Context:** Issue #1 Task 0.2 prescribes Google Benchmark via CMake
  `FetchContent`; Task 7.2 mentions pybind11 for the Python extension.
  FetchContent requires network access **at configure time**; the reference
  build environment (and possibly future contributor machines/CI runners
  behind proxies) cannot be assumed to have it. Phase 0's own acceptance
  criterion (Task 0.1) is only "build all four targets with zero warnings"
  — no benchmarking is required for it. The audit explicitly allows
  "pybind11 / Python C-API" for the extension target.
* **Decision:**
  1. Phase 0 scaffolding must configure and build **fully offline**:
     no FetchContent, no external C++ dependencies, no network.
  2. The `pdftoolkit_py` extension target uses the **raw Python C-API**
     for Phase 0 (module: `pdftoolkit_native`), not pybind11.
  3. Native tests use a **minimal in-repo assert harness**
     (`native/tests/test_harness.hpp`) until GoogleTest lands with the
     benchmark pipeline (Task 0.2). The harness is throwaway by design.
  4. When Task 0.2 is implemented, benchmark/test frameworks arrive behind
     a CMake option (default ON in CI, OFF for offline builds) — policy
     recorded as unknown U-006.
* **Alternatives considered:**
  * *Vendoring GoogleTest/GoogleBenchmark into the repo now* — rejected:
    hundreds of MB of third-party code for scaffolding value.
  * *FetchContent from the start* — rejected: breaks offline builds and
    makes first-configure fail on restricted networks.
  * *pybind11 now* — rejected for Phase 0: adds a heavy dependency for a
    version-string-level extension; the raw C-API is stable, documented,
    and sufficient until buffer-protocol ergonomics matter (U-004 keeps
    the Phase 7 switch open).
* **Consequences:**
  * Phase 0 verification is airtight: `cmake + ninja + ctest` run with
    no network, so any failure is a real defect, not a fetch glitch.
  * The test harness is deliberately minimal and will be deleted with the
    tests migrated to GoogleTest in Task 0.2 — do not grow it.
  * Switching to pybind11 later (if ever) does not change the C-ABI layer
    beneath it (audit Task 7.1 remains the stable contract).
