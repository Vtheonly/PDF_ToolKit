# ADR-0002 — Isolate all C++20 native code under `native/`

* **Status:** accepted (2026-10-09)
* **Supersedes:** none
* **Context:** Issue #1 (audit `docs/issues.md`) specifies a native C++20
  engine with paths like `include/pdftoolkit/memory/mmap.hpp` and
  `src/memory/mmap.cpp`, and a "root CMakeLists.txt". The repository
  already has a Python package rooted at `src/pdftoolkit/` via setuptools
  `package-dir = {"" = "src"}`. Implementing the audit paths literally
  (root-level `src/` + `include/`) would intermix C++ sources with the
  Python package tree: confusing layout, risk to setuptools package
  discovery, mixed-ecosystem tooling (pytest vs ctest) stepping on each
  other's globs, and no clean boundary for the licensing split (P-009:
  AGPL applies to the PyMuPDF-linked Python side, not to standalone C++).
* **Decision:**
  1. All native code (headers, sources, tests, native-only CMake logic)
     lives under `native/`:
     `native/include/pdftoolkit/…`, `native/src/…`, `native/tests/…`,
     with the real build logic in `native/CMakeLists.txt`.
  2. The root `CMakeLists.txt` is a **thin shim** that only
     `add_subdirectory(native)` — satisfying the audit's "root CMakeLists"
     requirement without entangling ecosystems.
  3. Audit path mapping rule (recorded in AGENTS.md §5.5 and
     `docs/architecture/native-tree.md`): audit `include/pdftoolkit/X` →
     `native/include/pdftoolkit/X`; audit `src/<module>/X.cpp` →
     `native/src/<module>/X.cpp`.
  4. Coexistence rules (additive-only, no shared state, honest stubs) are
     recorded in `docs/architecture/native-tree.md`.
* **Alternatives considered:**
  * *Literal audit paths at repo root* — rejected for the collision and
    licensing-boundary reasons above.
  * *Separate repository for the native core* — rejected: the audit and
    issue #1 are explicitly about evolving THIS repository; splitting
    would break the single-branch continuity model and complicate the
    task registry.
* **Consequences:**
  * One-line audit-path translation is permanently required when reading
    the audit — documented in three places to make it frictionless.
  * The Python packaging pipeline is structurally untouched by native work.
  * `git clean`/`.gitignore` needs to cover `native/build/` (done).
