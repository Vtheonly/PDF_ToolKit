# Native Engine — Target Tree & Coexistence Model (issue #1)

Status: **Phase 0 + Phase 1 landed (tasks 0.1–0.3, 1.1–1.3 done); Phase 2
underway (tasks 2.1 + 2.2 done)**. The Python engine remains the authoritative
implementation until the native core reaches parity and the audit's
performance gates pass (AGENTS.md §5.6).

## What exists today (verified 2026-10-09)

```
native/
├── CMakeLists.txt              real build logic: 4 targets + 10 ctests + benchmarks
│                               (PDTK_ENABLE_BENCHMARKS, default ON — ADR-0005;
│                               POSITION_INDEPENDENT_CODE ON — TLS in the .so, ADR-0006)
├── cmake/CompilerWarnings.cmake  zero-warning profile (-Werror)
├── include/pdftoolkit/
│   ├── pdftoolkit.h            C-ABI: 6 PDTK_API functions, panic-proof
│   ├── errors.hpp              ErrorCode enum + PdfToolkitException (typed core
│   │                           exceptions; never crosses extern "C")
│   ├── version.hpp             native_version() -> "0.1.0"
│   ├── memory/
│   │   ├── arena.hpp           BumpArena (task 1.2 done, measured via task 0.2)
│   │   ├── mmap.hpp            MmapHandle (task 1.1 done — guarded SIGBUS design,
│   │   │                       ADR-0006; immutable spans; madvise hints)
│   │   └── page_slab.hpp       UPS layout: PageSlabHeader (64 B exactly — audit's
│   │                           reserved[12] corrected to [8], P-015), builder +
│   │                           zero-copy PageSlabView (task 1.3 done)
│   └── parser/
│       ├── trailer.hpp         locate_startxref + TrailerInfo (task 2.1 done —
│       │                       tail_window tunable per P-016; tests-only
│       │                       detail::set_search_force_scalar hook)
│       └── xref.hpp            XRefIndex + XRefEntry (task 2.2 done — dual-mode
│                               resolver; from_linear_scan() trust flag)
├── src/
│   ├── core/{version,errors}.cpp
│   ├── memory/
│   │   ├── arena.cpp           cross-platform aligned alloc
│   │   ├── mmap.cpp            POSIX mmap + per-slot-seqlock guard registry +
│   │   │                       Win32 compile-only path (U-011)
│   │   └── page_slab.cpp       slab builder (32-B-aligned sections, CRC-32) +
│   │                           view validation (structure; crc pins bytes)
│   ├── parser/
│   │   ├── scan_util.hpp       INTERNAL shared primitives: char classes,
│   │   │                       string/array/dict skippers, skip_value,
│   │   │                       for_each_dict_entry (2.1+2.2 reuse; 2.3 seed)
│   │   ├── trailer.cpp         backward AVX2 (runtime-dispatched) + scalar
│   │   │                       needle search; tolerant offset parse;
│   │   │                       string/nesting-aware trailer dict extraction
│   │   └── xref.cpp            classic tables + xref streams (/W, /Index,
│   │                           type 0/1/2) + /Prev chains + /XRefStm hybrids
│   │                           + emergency linear scan + 10M-object cap
│   ├── ffi/pdftoolkit.cpp      EngineHandle + C-ABI implementations;
│   │                           register_document mmaps via MmapHandle (task 1.1)
│   ├── python/pdftoolkit_native.cpp  raw C-API extension module
│   └── cli/main.cpp            --version; subcommands arrive with 7.3
├── benchmarks/                 pdtk_bench_* (audit task 0.2, ADR-0005; a bench
│   ├── bench_arena.cpp           exists only when its component does — map
│   ├── bench_mmap.cpp            at the top of benchmarks/CMakeLists.txt)
│   └── bench_slab.cpp
└── tests/                      minimal harness (ADR-0003; migration deferred, U-009)
    ├── test_mmap.cpp           15 cases: truncation acceptance + honesty contract
    ├── test_slab.cpp           16 cases: alignment acceptance + tamper detection
    ├── test_trailer.cpp        31 cases: 10k-document acceptance corpus (×2 for
    │                           SIMD/scalar differential) + mmap zero-copy proof
    └── test_xref.cpp           18 cases: 2k-document corpus (classic/incremental/
                                linearized/stream/hybrid/corrupt/Flate) + OOM guard
```

Sibling directories from the same audit task (not native code, so they
live outside `native/`, ADR-0002):

```
scripts/run_perf.sh            benchmark runner + perf stat stage (see U-010)
docs/benchmarks/                recorded results — the only citable perf numbers
```

Verified: zero-warning build (GCC 14.2, C++20), ctest 10/10 (release;
the sanitizer presets run 9/9 — python_import_smoke is excluded there
by design),
`nm -D` shows exactly the 6 C symbols exported from the FFI library,
extension imports from Python, CLI runs. Guarded-mmap acceptance:
truncation under a live mapping throws `PdfToolkitException(IoTruncated)`
instead of crashing (ADR-0006, test-locked). Slab acceptance: coordinate
offsets 32-byte aligned for AVX2 (task 1.3, test-locked; header exactly
one cache line after the P-015 correction). Trailer-scanner acceptance:
10,000-document deterministic corpus with variable whitespace and
trailing junk, offsets + `/Root` + `/Prev` all asserted, SIMD and scalar
paths agreeing (task 2.1, test-locked; synthetic-corpus interpretation
U-013, window tunable P-016). XRef acceptance: 2,000-document corpus
across classic tables, incremental /Prev chains, linearized-style
layouts, xref streams, hybrid /XRefStm files, corrupt tables and Flate
degradation — every expected entry asserted, emergency linear scan
flagged (task 2.2, test-locked; Flate dependency inversion P-017,
vector<uint64_t> underspecification P-017b).

## Isolation model (ADR-0002)

```
repository root
├── CMakeLists.txt          ← thin shim: add_subdirectory(native) ONLY
├── pyproject.toml          ← Python packaging (unchanged)
├── scripts/run_perf.sh     ← benchmark runner (audit task 0.2)
├── docs/benchmarks/        ← recorded benchmark results (citable numbers)
├── src/pdftoolkit/         ← Python engine (production)
└── native/                 ← ALL C++20 code lives here
    ├── CMakeLists.txt      ← real build logic
    ├── include/pdftoolkit/ ← public headers (audit: include/pdftoolkit/*)
    ├── src/                ← implementation (audit: src/*)
    ├── benchmarks/         ← pdtk_bench_* targets (audit: benchmarks/*)
    └── tests/              ← native tests (ctest-registered)
```

Why: the audit's literal paths (`include/pdftoolkit/...`, `src/memory/...`)
would collide with the Python package's `src/` layout and setuptools'
package discovery. Mapping rule (AGENTS.md §5.5):

```
audit path                    →  repository path
include/pdftoolkit/X.hpp      →  native/include/pdftoolkit/X.hpp
src/<module>/X.cpp            →  native/src/<module>/X.cpp
```

## Build targets (audit Task 0.1)

| Target | Type | Purpose |
|--------|------|---------|
| `pdftoolkit_core` | static lib | C++20 systems core (memory, parser, codec, font, layout, index, search, geometry, ops, runtime) |
| `pdftoolkit_ffi` | shared lib | pure C-ABI surface (`pdftoolkit.h`); exceptions never cross the boundary |
| `pdftoolkit_py` | Python extension | Buffer-Protocol zero-copy bindings; Phase 0 uses raw C-API (ADR-0003) |
| `pdftoolkit_cli` | executable | native headless CLI (Arrow IPC / JSON lines on stdout) |

Toolchain: GCC ≥ 13 / Clang ≥ 16 / MSVC 2022, C++20 (`cxx_std_20`).
Warnings are errors: `-Wall -Wextra -Wpedantic -Wconversion` (GCC/Clang),
`/W4 /permissive-` (MSVC). Release optimization per audit §Task 0.1
(`-O3`, optional `-march=native` behind `PDTK_ENABLE_NATIVE_ARCH`, U-003).

## Planned module map (from the audit; populate as phases land)

```
native/include/pdftoolkit/
├── version.hpp                    (task 0.1 — version macros)
├── errors.hpp                     (task 0.1 — ErrorCode enum, C-mappable)
├── pdftoolkit.h                   (task 7.1 — pure C-ABI header)
├── memory/   mmap.hpp (1.1 done) · arena.hpp (1.2 done) · page_slab.hpp (1.3 done)
├── parser/   trailer.hpp (2.1 done) · xref.hpp (2.2 done) · lexer.hpp (2.3)
├── codec/    flate.hpp (2.4)
├── font/     cmap.hpp (3.1)
├── layout/   evaluator.hpp (3.2) · materializer.hpp (3.3)
├── index/    inverted.hpp (4.1)
├── search/   wand.hpp (4.2) · simd.hpp (4.3)
├── geometry/ spatial.hpp (4.4)
├── ops/      plan.hpp (5.1) · writer.hpp (5.2)
└── runtime/  registry.hpp (6.1) · pool.hpp (6.2) · watchdog.hpp (6.3)
```

## Coexistence rules

1. **No shared state**: the native core never imports or wraps the Python
   engine and vice versa; integration happens only at the FFI/extension
   boundary (Phase 7) or the CLI.
2. **Additive only**: native scaffolding must never stub, delete or
   degrade Python capabilities (dos-and-donts.md).
3. **Honest stubs**: C-ABI functions without backing implementations
   return `PDTK_ERR_NOT_IMPLEMENTED`; success is never faked.
4. **Single CMake entry**: the root `CMakeLists.txt` must remain a thin
   shim so Python tooling (pip/setuptools) is unaffected by C++ builds.
