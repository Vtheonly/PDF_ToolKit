# ADR-0007 — libdeflate as the Flate engine, fetched behind an option (task 2.4)

* **Status:** accepted (2026-10-09)
* **Context:** audit issue-1/task 2.4 — "Replace general-purpose zlib
  with a hardware-accelerated Flate decoder", prescribing
  "`libdeflater` via CMake FetchContent", wrapped in
  `src/codec/flate.cpp`, decompressing into the thread's `BumpArena`,
  with decompression-bomb defenses (metadata length check, 128x
  expansion ratio, ~64 MB per-page budget) and a >= 800 MB/s/core
  benchmark gate.
* **Supersedes:** none. **Amends:** ADR-0003's "zero external C++
  dependencies" consequence — this is the first RUNTIME dependency of
  `pdftoolkit_core` (google/benchmark, ADR-0005, is a benchmark-only
  dependency), so the offline guarantee needs a new escape hatch.

## Decision

1. **The library is `libdeflate` (ebiggers), v1.26, not "libdeflater".**
   The audit's name does not resolve to a C library
   (`github.com/ebiggers/libdeflater` does not exist; "libdeflater" is
   the name of a Rust *binding crate* for libdeflate). The intent — a
   SIMD-accelerated Flate decoder integrable via CMake FetchContent —
   is unambiguous, and libdeflate is the canonical implementation
   (runtime CPU dispatch incl. AVX2; zlib/gzip/deflate containers;
   MIT-style license, no AGPL boundary concern per AGENTS.md §8).
   Filed as an audit-text defect (**P-019**), same class as
   P-011/P-015.

2. **`PDTK_ENABLE_FLATE` CMake option, default ON — the ADR-0005
   pattern applied to a runtime dependency.** ON = configure fetches
   libdeflate (pinned release tag v1.26; bumps deliberate, never a
   floating branch), builds it as a static library with programs,
   tests, shared-lib and install rules all OFF, and links it PRIVATEly
   into `pdftoolkit_core` (the codec header is PIMPL'd — no
   third-party type leaks into the public API). OFF = `flate.cpp`
   compiles as an honest stub: `supported() == false`, every
   `decompress()` returns `FlateStatus::Unavailable`, and callers keep
   their degradation paths (the xref resolver's emergency linear
   scan). The ADR-0003 offline guarantee is preserved by construction:
   `cmake -B build -DPDTK_ENABLE_FLATE=OFF -DPDTK_ENABLE_BENCHMARKS=OFF`
   touches no network (verified: no `_deps` directory, 12/12 ctest).
   `PDTK_HAVE_FLATE=1|0` is exported as a PUBLIC compile definition so
   tests and benchmarks branch on the real capability, never a guess.

3. **Sanitizer presets keep Flate ON** (unlike benchmarks, ADR-0005
   point 2): the codec is core functionality whose parsing paths ASan/
   TSan should cover — including the fetched third-party code, which
   is compiled with the preset's sanitizer flags.

4. **The benchmark exists only when the codec does** (component-exists
   rule, P-012): `pdtk_bench_flate` is added under
   `if(PDTK_HAVE_FLATE)` — a stub must never produce numbers.

5. **Arena staging, not caller buffers.** Decompression output goes
   into the caller's `BumpArena` (audit step 3) via speculative chunks
   (8x compressed, or the caller's trusted metadata hint) with
   rewind-and-retry growth, per the recording
   `docs/benchmarks/2026-10-09-phase2-flate-baseline.md` (the 8x first
   guess is a measured tunable: a 4x guess wastes a full aborted
   attempt on every ~5x-ratio content stream). This required one
   minimal `BumpArena` extension — `rewind_to(mark)` (speculative
   sub-allocation), task-1.2's component extended, not forked.

## Alternatives considered

* **System zlib** — rejected: the audit explicitly asks to replace
  general-purpose zlib; zlib inflate is ~2-3x slower than libdeflate's
  SIMD paths and would leave the 800 MB/s gate at risk on throttled
  cores.
* **Vendoring libdeflate into `native/`** — rejected (ADR-0003
  precedent: no vendored third-party code; ~1 MB of update liability).
* **Unconditional FetchContent (no option)** — rejected: would break
  the offline guarantee for air-gapped builds; the option costs one
  flag and keeps ADR-0003 intact by construction.
* **zlib-ng instead of libdeflate** — rejected: same class of
  performance, but a wider API surface than needed (we only inflate)
  and no advantage on the zlib-container requirement; libdeflate's
  one-shot API maps exactly onto the arena-staging design.
* **Streaming inflate into a growable std::vector** — rejected:
  violates the audit's step 3 (arena memory) and adds a copy plus
  allocator traffic on the ingestion hot path.

## Consequences

* First configure with `PDTK_ENABLE_FLATE=ON` needs network (like
  ADR-0005); incremental builds use the `_deps` cache. CI's
  native-release/asan/tsan jobs fetch it (runners have network).
* Version bumps of libdeflate are deliberate commits (pin in
  `native/CMakeLists.txt`), same discipline as google/benchmark.
* Offline/profile builds honestly lack Flate: xref streams with
  `/FlateDecode` degrade to the linear scan exactly as before task
  2.4 (P-017a's resolution applies to default builds).
* `/DecodeParms` predictors (PNG/TIFF) remain unsupported and degrade
  honestly — deliberate scope line, tracked as P-020 (silently
  mis-decoding predictor rows would be far worse than a flagged scan).
