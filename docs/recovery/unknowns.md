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
* **Assumption:** `ubuntu-latest` (24.04) default GCC ≥ 13 handles
  ASan/UBSan/TSan; MSan needs an explicit clang install step.
* **How to resolve:** First live CI run of `.github/workflows/ci.yml`
  (issue-1/task-0.3). Until then MSan remains locally unverified (P-008).

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
* **Assumption (current):** acceptable in CI (GitHub runners have network)
  and for developers; local offline builds stay possible with benchmarks
  disabled via a CMake option (ADR-0003).
* **How to resolve:** when implementing issue-1/task-0.2.

## U-007 — Git history below the v2.0.0 squash points

* **Question:** The five original applications' individual histories are
  not in this repository (log shows 9 commits). Is there an upstream
  archive to consult for behavioural archaeology beyond README/ARCHITECTURE
  tables?
* **Assumption:** No; README's consolidation table and the regression
  tests are the complete behavioural specification.
* **How to resolve:** ask the repository owner if a question ever requires
  original-source archaeology.
