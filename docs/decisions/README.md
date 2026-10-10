# Decision Records (ADRs)

Every irreversible or future-constraining engineering decision gets an
ADR. Format: Michael Nygard style — Context, Decision, Alternatives
considered, Consequences.

Rules:

* Numbering: `adr-NNNN-slug.md`, monotonically increasing (0001–0008 exist).
* An ADR is never edited to reverse itself — a new ADR supersedes it and
  both remain (status field flips to `superseded by ADR-NNNN`).
* Every ADR must be linked from the relevant code location's documentation
  and from `docs/recovery/change-log.md`.

## Index

| ADR | Title | Status |
|-----|-------|--------|
| [0001](adr-0001-single-branch-policy.md) | Single-branch (`main` only) repository policy | accepted |
| [0002](adr-0002-native-tree-isolation.md) | Isolate all C++20 native code under `native/` | accepted |
| [0003](adr-0003-phase0-offline-build-minimal-deps.md) | Phase 0 builds offline with zero external C++ dependencies | accepted |
| [0004](adr-0004-python-keyword-scan-optimization-rejected.md) | Reject Python-level combined-keyword scan optimization | accepted |
| [0005](adr-0005-benchmark-pipeline-policy.md) | Benchmark pipeline policy (FetchContent pin, single-invocation rule, recording ledger) | accepted |
| [0006](adr-0006-guarded-mmap-sigbus-design.md) | Guarded SIGBUS recovery for MmapHandle (sigsetjmp/siglongjmp, not throw-from-handler) | accepted |
| [0007](adr-0007-libdeflate-integration-policy.md) | libdeflate as the Flate engine, fetched behind `PDTK_ENABLE_FLATE` (ADR-0005 pattern for a runtime dep) | accepted |
| [0008](adr-0008-xxhash64-in-repo.md) | xxHash64 implemented in-repo (internal header, reference-vector pinned; no third fetchable dependency) | accepted |
| [0009](adr-0009-evaluator-precision-model.md) | Evaluator exposes float32 Mat6, carries double internally (f32 accumulation measured past the +/-0.001 pt gate: 1.309e-03 pt / 400 glyphs) | accepted |
