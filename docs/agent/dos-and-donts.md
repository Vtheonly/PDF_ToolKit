# Agent DOs and DON'Ts

Distilled from real defects, past regressions and session pitfalls of this
repository. Violating a DON'T has caused, or would have caused, actual bugs.

## Build & repository

* **DO** work directly on `main` and push after every commit.
* **DON'T** create feature branches, PR stacks, or leave unpushed commits —
  the repo runs a single-branch policy (verified: only `main` exists).
* **DON'T** force-push or rewrite history.
* **DON'T** commit `.venv/`, `build/`, `__pycache__/`, `*.egg-info/` —
  keep them ignored/cleaned.
* **DO** use the repo virtualenv (`.venv/`). The system Python is
  PEP-668-managed and refuses global installs.
* **DO** remember cmake/ninja may only exist inside `.venv` (pip-installed).

## Python engine (`src/pdftoolkit/`)

* **DO** keep every `import fitz` inside `io/pdfio.py` — it is the single
  PDF backend boundary (one-file swap is the design goal).
* **DON'T** build result envelopes outside `Toolkit._run` — envelope drift
  between CLI/HTTP/Python adapters was a real defect class in the
  predecessor project (`pdftoolkit_2`).
* **DON'T** catch exceptions to "make things pass" — map them to the typed
  error hierarchy (`core/errors.py`); unknown errors become
  `OperationError` at the facade, not inside services.
* **DON'T** weaken the deliberate hardening: output-conflict detection,
  strict page specs, refusal of 0-page/encrypted/junk PDFs, idempotent
  `merge_folder`, exactly-one speech output mode, upload sandboxing. Each
  rule exists because an original application lacked it.
* **DON'T** "optimize" `count_keywords` into a combined multi-keyword
  regex — it cannot preserve per-keyword non-overlapping count semantics
  (ADR-0004 explains the two failure modes). The real fix is the native
  inverted index (issue #1, Task 4.1).
* **DO** add regression tests when fixing bugs — the 644-test suite is the
  engine's safety net; a fix without a test is not verified.
* **DO** keep tests self-contained (build PDFs with `tests/helpers.py`;
  no binary fixtures, no network, no audio hardware).

## Native engine (`native/`)

* **DO** mirror the audit's module layout under `native/include/pdftoolkit/`
  and `native/src/` (memory, parser, codec, font, layout, index, search,
  geometry, ops, runtime, ffi).
* **DON'T** mix C++ sources into `src/pdftoolkit/` (Python) or create a
  root-level `src/`+`include/` pair that collides with the Python package
  layout (ADR-0002).
* **DO** treat warnings as errors: `-Wall -Wextra -Wpedantic -Wconversion`
  (GCC/Clang), `/W4` (MSVC).
* **DON'T** let C++ exceptions cross `extern "C"` boundaries — wrap every
  FFI body in `try/catch (...)` → integer error codes (audit Task 7.1).
* **DON'T** claim performance numbers without a benchmark artifact and the
  command that produced it.
* **DO** gate wall-clock assertions on optimized, uninstrumented builds:
  sanitizers cost ~100x (measured 0.42 ms → 41.6 ms under ASan) and -O0
  ~50x on allocation-heavy loops. Use the `PDTK_TIMING_GUARDS_OFF`
  pattern from `native/tests/test_arena.cpp` (`__has_feature` /
  `__SANITIZE_*` + `NDEBUG`). Functional assertions stay active in every
  build.
* **DO** keep stubs honestly labelled: FFI functions without backing
  implementations return `PDTK_ERR_NOT_IMPLEMENTED`; never fake success.
* **DON'T** delete or stub Python engine capabilities while their native
  replacements are unfinished — the Python engine stays authoritative
  until parity + performance gates pass.

## Documentation & evidence

* **DO** update task registry, problem registry, change log and affected
  ADRs in the same commit as the code.
* **DON'T** mark anything done/verified without a captured command + result.
* **DON'T** leave newly discovered knowledge only in chat/commit messages —
  it must reach the registry documents.
* **DO** record environment-specific verification limits (e.g. MSan needs
  clang → CI-only) instead of silently skipping them.
* **DON'T** restate stale counts (tests, coverage, versions) — re-measure
  and update (the README claimed 646 tests while the suite had 644; that
  drift is exactly what this rule prevents).
