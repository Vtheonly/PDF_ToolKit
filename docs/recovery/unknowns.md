# Unknowns — Open Questions, Assumptions, Uncertainties

Anything not (yet) verifiable in the current environment or requiring a
future decision. Each entry: `U-NNN`, question, current assumption, how to
resolve. When resolved, move the answer to the relevant doc and mark the
entry resolved with a pointer.

## U-001 — Why did the documented test count drift from 646 to 644?

* **Question:** Which commit removed/merged the two tests without updating
  the README/ARCHITECTURE prose?
* **Assumption:** Housekeeping during the v2.0.0 unification commits
  (`95fc701`, `661caf2` era) — prose was written against an earlier count.
* **How to resolve:** `git log -p --follow tests/` archaeology if it ever
  matters; the practical rule (re-measure counts) is already in force.

## U-002 — CI runner toolchain for the sanitizer matrix

* **Question:** Which GitHub-hosted runner image provides clang 16+ for the
  MemorySanitizer preset, and does `ubuntu-latest`'s default GCC support
  `-fsanitize=thread` cleanly under ctest?
* **Status:** `resolved` for the GCC part — **first live CI run succeeded**
  (run 37820960657 on a9c5e98, 2026-10-09): all 5 job instances green
  (pytest py3.9, pytest py3.12, native release, native asan, native
  tsan) on `ubuntu-latest`; the `vm.mmap_rnd_bits=28` mitigation worked
  (or was not needed — it is harmless either way).
* **Still open:** the MSan-with-clang job — needs a clang toolchain plus
  an MSan-instrumented libc++ strategy on the runner. Not wired into CI
  yet; pursue only when Phase 1+ code justifies the setup cost.

## U-003 — `-march=native` default vs distributable builds

* **Question:** Should release builds default to `-march=native` (audit
  text implies it) when artifacts must run on other machines?
* **Assumption (current):** expose it as a CMake option
  (`PDTK_ENABLE_NATIVE_ARCH`), default ON for local/CI builds per the
  audit, OFF for any future distribution packaging.
* **How to resolve:** packaging story for the native artifacts (post
  Phase 7) — revisit then.

## U-004 — pybind11 vs raw Python C-API for `pdftoolkit-py` (Phase 7)

* **Question:** Final binding technology for the zero-copy extension.
* **Assumption (current):** Phase 0 scaffolding uses the raw C-API to keep
  the build offline/dependency-free (ADR-0003); Phase 7 may switch to
  pybind11 if ergonomics demand it, without changing the C-ABI beneath it.
* **How to resolve:** at issue-1/task-7.2 with buffer-protocol benchmarks
  on both options.

## U-005 — Python version matrix for the native extension

* **Question:** Which Python versions must `pdftoolkit_py` support
  (`pyproject.toml` says `>=3.9`; local env is 3.12.14; the C-API ABI is
  version-specific)?
* **Assumption:** CI matrix over 3.9–3.13 once the extension has real
  functionality; Phase 0 builds only against the venv interpreter.
* **How to resolve:** when wiring the extension into CI (task 0.3
  follow-up or 7.2).

## U-006 — FetchContent network policy for Google Benchmark / GoogleTest

* **Question:** Is configure-time network access acceptable for CI and
  developer machines (FetchContent downloads), or must the repo vendor
  dependencies?
* **Status:** `resolved` (2026-10-09, task 0.2) — policy ADR-0005:
  `PDTK_ENABLE_BENCHMARKS` option, default ON (CI and developer machines
  have network); offline machines configure with OFF, which skips the
  FetchContent entirely (verified: no `_deps` produced, full suite green).
  Sanitizer presets keep benchmarks OFF. GoogleTest remains unfetched —
  see U-009.

## U-009 — When does the native test harness migrate to GoogleTest?

* **Question:** ADR-0003 predicted the in-repo assert harness
  (`native/tests/test_harness.hpp`) would be replaced by GoogleTest when
  task 0.2 landed. Task 0.2 shipped the *benchmark* pipeline only (scope:
  the audit's task-0.2 text is about benchmarking; ADR-0005 records the
  correction). When is GoogleTest actually warranted?
* **Assumption (current):** at the first test need the harness cannot
  express — expected triggers: Phase 2 parser corpora (value-parameterized
  tests over malformed-PDF corpora) or expected-error-path tests
  (task 5.1 `std::expected`). Until then the harness stays minimal
  (ADR-0003: do not grow it).
* **How to resolve:** at the first such test, fetch GoogleTest behind the
  same `PDTK_ENABLE_BENCHMARKS`-style option pattern (ADR-0005), migrate
  the existing tests, and delete the harness in that same commit.

## U-010 — `perf stat` unavailable: L1-miss/IPC scorecard rows need bare metal

* **Question:** The audit's scorecard requires `L1-dcache-load-misses`,
  `instructions`, `cycles` (rows: L1 data cache miss rate, IPC). `perf` is
  not installed in the reference environment (verified 2026-10-09:
  `command -v perf` → nothing) and hosted CI runners typically block
  user-space profiling via `kernel.perf_event_paranoid` (expected on
  ubuntu-latest; confirm from the first CI run's log).
* **Assumption (current):** latency benchmarking must not depend on PMU
  access. `scripts/run_perf.sh` probes perf functionally
  (`perf stat -e instructions -- true`) and degrades to a printed
  `SKIPPED (<reason>)` with exit 0 — verified live for the
  not-installed case.
* **How to resolve:** when a self-hosted/privileged runner exists, re-run
  the suite there and record the counter rows in `docs/benchmarks/`;
  until then the two scorecard rows stay honestly blocked, not faked.

## U-007 — Git history below the v2.0.0 squash points

* **Question:** The five original applications' individual histories are
  not in this repository (log shows 9 commits). Is there an upstream
  archive to consult for behavioural archaeology beyond README/ARCHITECTURE
  tables?
* **Assumption:** No; README's consolidation table and the regression
  tests are the complete behavioural specification.
* **How to resolve:** ask the repository owner if a question ever requires
  original-source archaeology.

## U-008 — MSVC `_aligned_malloc` path is compile-only until CI runs

* **Question:** Does `BumpArena`'s `#ifdef _MSC_VER` allocation path
  (`_aligned_malloc` / `_aligned_free`) compile and behave correctly?
* **Assumption (current):** yes — it is the canonical MSVC idiom and is
  symmetrical with the POSIX path; but the reference environment has GCC
  only, so it has never been compiled here.
* **How to resolve:** first Windows CI run (task 0.3's matrix); the
  pure-C smoke test and arena tests must pass there unchanged.

## U-011 — Windows SEH translation of EXCEPTION_IN_PAGE_ERROR is compile-only

* **Question:** Does the Win32 `MmapHandle` path
  (CreateFileA + CreateFileMappingA + MapViewOfFile) behave correctly,
  and can `guarded()` ever protect accesses on Windows?
* **Assumption (current):** the mapping path compiles and works (it is
  the canonical Win32 idiom, mirroring U-008's MSVC precedent), but
  `guarded()` runs its body unguarded: SIGBUS recovery is POSIX-only and
  the Win32 analogue — an SEH `__except` filter translating
  `EXCEPTION_IN_PAGE_ERROR` — is deferred. `advise()` returns false on
  Windows (PrefetchVirtualMemory is the candidate wiring).
* **How to resolve:** first Windows CI run compiles and exercises the
  mapping; SEH translation is a deliberate follow-up decision (needs an
  ADR of its own — mixing SEH and C++ exceptions has its own rules).

## U-013 — No real-world PDF corpus in-repo; native acceptance tests use synthetic documents

* **Question:** The audit's task-2.1 acceptance names "10,000 PDF test
  files". The repository deliberately carries no binary fixtures
  (AGENTS.md §4.8: tests build their own PDFs; no binary fixtures on
  disk) and no external corpus is vendored. How should PDF-parser
  acceptances be satisfied, and how do we know the synthetic documents
  exercise the same paths real files do?
* **Assumption (current):** a deterministic synthetic corpus
  (LCG-seeded, 10,000 documents for task 2.1) varying every axis the
  acceptance names (whitespace mix, trailing junk, decoys, optional
  entries) satisfies the criterion's intent — the same approach the
  Python engine's 644-test suite takes. Known gap: synthetic documents
  only contain defects and shapes the generator thought of. Real-world
  corpora (e.g. the pdf.js/mupdf test sets, Ghostscript's corpus)
  contain stranger pathologies (hybrid XRef chains, object streams
  referring to deleted objects, 1-byte files, encodings).
* **How to resolve:** when a corpus becomes available (downloaded at
  test time is against the offline-build principle — so likely a
  separately-fetched developer-only corpus), run a differential test:
  native scanner vs PyMuPDF (`fitz.Document.xref_length()` /
  trailer access) over the same files, asserting matching `startxref`
  offsets and `/Root` references. Until then the synthetic corpus is
  the honest, reproducible evidence.

## U-012 — Absolute throughput gates exceed the reference environment's ceiling

* **Question:** Can the audit's later throughput acceptance gates be
  validated in this environment at all — task 2.3 lexer "> 2.5 GB/s",
  task 4.3 SIMD scanner "> 4.0 GB/s"?
* **Assumption (current):** no. The task-1.1 baseline's control
  experiment measured a **~2.9 GB/s ceiling for any 128 MiB traversal in
  this sandbox** — even a plain anonymous heap buffer with a
  vectorized (AVX-512) byte-sum (2.40–2.85 GB/s; warm mmap 88–90 % of
  that; recording: `docs/benchmarks/2026-10-09-phase1-mmap-baseline.md`).
  The sandbox is a shared-vCPU kata container; the gates were surely
  drafted against real hardware.
* **How to resolve:** when the gated tasks land, report the ratio
  "component throughput / environment traversal ceiling" alongside the
  absolute number, and mark gate pass/fail as environment-bound unless a
  self-hosted runner is available. The durable cross-environment metric
  for task 1.1 is already recorded (mmap retains ~88–90 % of heap speed).
* **Update (2026-10-09, task 2.3):** the ceiling itself is RUN-DEPENDENT,
  not just environment-dependent: the same day, the phase-1 control
  measured 2.40–2.85 GB/s while the task-2.3 control measured
  4.50–4.78 GB/s (~1.7×). Always re-measure the control in the SAME run
  as the gated component (the lexer baseline does exactly this); never
  ratio a component against a control from an earlier session.

## U-014 — "Standardized glyph-test document" acceptance interpreted as a deterministic synthetic corpus

* **Question:** the audit's task-3.1 acceptance says "Accurate decoding
  of complex CID-keyed and TrueType subset fonts verified against a
  standardized glyph-test document." No such standardized document
  exists in the repository (no binary fixtures by policy, AGENTS.md
  §4.8), and the audit names none. What satisfies the criterion?
* **Assumption (current):** the same approach as U-013 — a
  deterministic generator IS the glyph-test document: 500 synthetic
  font ToUnicode programs (CID-keyed with contiguous + array bfranges,
  surrogate-pair and ligature targets; TrueType subsets with sparse
  bfchars), each with a coded probe string and its exact expected
  UTF-32 computed independently by the generator, decoded through the
  production intern path (`acceptance_glyph_test_corpus`). Known gap:
  the generator only contains font shapes the generator thought of.
* **How to resolve:** when a real glyph-test corpus becomes available
  (Adobe's CMap test files, the pdf.js font suite, or a
  PyMuPDF-differential over real PDFs), run the decode comparison and
  record it; until then the synthetic corpus is the honest,
  reproducible evidence.

## U-015 — "Adobe Acrobat reference output" interpreted as an independent synthetic golden model

* **Question:** the audit's task-3.2 acceptance reads "Graphics and
  text matrix calculations match Adobe Acrobat reference output to
  within +/-0.001 point precision." No Acrobat output exists in the
  repository (no binary fixtures by policy, AGENTS.md 4.8), Acrobat
  itself is proprietary and unavailable in this environment, and the
  audit names no reference document. What satisfies the criterion?
* **Assumption (current):** the same approach as U-013/U-014 — a
  deterministic generator IS the reference: `scripts/
  gen_evaluator_golden.py` models ISO 32000-1:2008 cl. 9 semantics
  INDEPENDENTLY of the C++ evaluator (emission and evaluation fused so
  stream and expectations cannot drift; operands parsed as binary32 to
  match the task-2.3 lexer, algebra in binary64 per ADR-0009; seed
  0x3D2E1F). Its 12 scenarios x 605 golden glyphs (rotated Tm, CTM
  q/Q stack, kerned TJ, 2-byte CID fonts, Ts/Tz/Tc/Tw, graphics
  noise, a 400-glyph drift probe, evaluate() continuation) gate the
  native implementation to 1e-3 pt per glyph Trm field, advance and
  final matrix (`golden_gate_within_0_001_pt`). Known gap: the
  generator only contains operator shapes the generator thought of.
* **Precision note:** Acrobat's own arithmetic is 16.16 fixed point
  (~1.5e-5 pt resolution), not float32 — the gate never implied
  float32 accumulation suffices (see ADR-0009's measured 1.309e-03 pt
  f32 drift on the 400-glyph probe).
* **How to resolve:** when a real-Acrobat reference (or a
  PyMuPDF/pdf.js differential over real PDFs) becomes available, run
  the coordinate comparison and record it as a second reference
  column; until then the synthetic golden model is the honest,
  reproducible evidence.
