# AGENTS.md — Permanent Agent Instructions for PDF_ToolKit

This file is the **entry point for every agent (human or AI) working on this
repository**. It was created as the first iteration of the project's permanent
agent instructions during the session that started work on
[issue #1](https://github.com/Vtheonly/PDF_ToolKit/issues/1) (the native
C++20 re-architecture epic; the full audit lives in `docs/issues.md`).

Read this file completely **before making any change**. If any instruction
here conflicts with a newer decision in `docs/decisions/`, the decision
record wins and this file must be updated as part of that change.

Companion guides live in `docs/agent/`:

| Guide | Contents |
|-------|----------|
| `docs/agent/README.md` | Index of the agent documentation system |
| `docs/agent/workflows.md` | The mandatory task lifecycle, commit format, push discipline |
| `docs/agent/dos-and-donts.md` | Hard DOs and DON'Ts distilled from real mistakes |

State you must know before working is kept in:

| Document | Contents |
|----------|----------|
| `docs/recovery/task-registry.md` | Live status of every issue-#1 task: what is done, in progress, blocked, with evidence links |
| `docs/recovery/problem-registry.md` | Known bugs, architectural problems, technical debt, limitations |
| `docs/recovery/unknowns.md` | Open questions, assumptions, unverified areas |
| `docs/recovery/change-log.md` | Significant discoveries and completed changes, newest first |
| `docs/architecture/` | Current-state architecture maps (Python engine + native tree) |
| `docs/benchmarks/` | Recorded benchmark results — the only citable performance numbers |
| `docs/decisions/` | ADRs: every irreversible engineering decision and its reasoning |

---

## 1. Golden rules

1. **Read before you write.** Read `AGENTS.md`, then the relevant entries in
   `docs/recovery/`, `docs/architecture/` and `docs/decisions/` before
   changing code. Never re-derive knowledge that is already documented.
2. **Evidence or it did not happen.** Never mark a task, fix or verification
   as complete without a reproducible command and its captured output. Every
   "done" entry in the task registry must name the exact verification command.
3. **Documentation is part of the work, not an afterthought.** A code change
   without its documentation updates (task registry, change log, affected
   ADRs) is an incomplete change and must not be committed.
4. **One logical change per commit, push after every commit.** The repository
   runs a **single-branch policy** (see §3). Commit directly to `main`, push
   immediately, and never leave `main` broken.
5. **Persist new knowledge immediately.** Any newly discovered problem,
   constraint, root cause, dependency or surprising behaviour goes into the
   appropriate registry in the same commit that discovers it. Do not leave it
   only in chat or memory — the next agent must not rediscover it.
6. **Reuse and extend; never fork logic.** Before implementing anything,
   inspect the existing implementation and its dependencies. Extend the
   existing primitive instead of creating a parallel implementation.
7. **Scope control.** Make meaningful, verified progress in small increments.
   Do not introduce architectural drift, speculative abstractions, or changes
   outside the task's stated scope.

## 2. Mandatory task lifecycle

Every task (from a GitHub issue, the audit epic, or a bug report) follows
this exact sequence — see `docs/agent/workflows.md` for the full detail:

1. Implement the fix/change (after inspecting existing logic and reusing it).
2. Test and verify it properly (run the relevant suite; capture evidence).
3. Update documentation with: what was wrong, why it happened, what changed,
   what was verified, what remains unresolved.
4. Mark the task completed in `docs/recovery/task-registry.md` (with evidence).
5. Update the relevant `docs/recovery/problem-registry.md` entry.
6. Update progress / next-task documentation if necessary.
7. Create a detailed Git commit (format in §6) and push it.

## 3. Repository & branch policy

* **Single branch: `main`.** All work lands on `main`. There are no feature
  branches, no release branches, no PR queues. (Verified 2026-10-09: the repo
  contains exactly one branch — `main` — and zero open pull requests.)
* **Push after every commit.** Never accumulate unpushed commits; a failed
  push must be resolved before the next task starts.
* **`main` is always green.** Never push a commit that leaves the Python test
  suite or the native build broken.
* **No history rewrites.** No force pushes, no rebases of pushed commits.
* Commit messages follow the format in §6.

## 4. Python engine rules (`src/pdftoolkit/` — the production engine)

The Python package is the **working, shipping engine** (v2.0.0). It is
protected by 644 tests (~97% coverage). Rules:

1. **Layering is law** (see `docs/architecture/python-engine.md` and
   `ARCHITECTURE.md`):
   `core/` ← `utils/`, `io/`, `documents/` ← `services/` ← `toolkit.py` ← `api/`.
   A layer may only import from layers below it. Nothing imports from
   `api/` or `toolkit.py`.
2. **`io/pdfio.py` is the ONLY module allowed to `import fitz`.** Swapping
   the PDF backend must remain a one-file change.
3. **Only `Toolkit._run` builds result envelopes.** Services return plain
   dicts and raise typed errors from `core/errors.py`. The envelope shape
   `{ok, operation, data, error, engine}` must never drift between adapters.
4. **Error taxonomy is frozen**: add new error classes only by subclassing
   `PdfToolkitError` with a stable `code` and `http_status`.
5. **The service catalog in `services/__init__.py` is the single source of
   truth** for capabilities; new capabilities = new service module +
   catalog entry + thin `Toolkit` method (+ optional one-line CLI/HTTP
   dispatch).
6. **Hardening behaviours are intentional** (see README "Deliberate
   hardening"): output-conflict detection, strict page specs, refusing
   0-page/encrypted PDFs, idempotent `merge_folder`, exactly-one speech
   output mode, upload sandboxing. Do not "fix" these away as bugs.
7. **`count_keywords` semantics are load-bearing**: per-keyword
   *non-overlapping* counts, both match axes (`case_sensitive`,
   `whole_words`). A combined multi-keyword regex CANNOT reproduce these
   semantics (leftmost-first alternation breaks substring-mode counts;
   lookahead scanning breaks non-overlap) — see ADR-0004. Do not attempt a
   Python-level "single-pass scan" optimization; the audit's inverted index
   in the native core is the correct fix.
8. **Tests build their own PDFs** via `tests/helpers.py` (no binary
   fixtures on disk). New tests must follow this pattern and stay
   deterministic and isolated.
9. **Any change to `src/pdftoolkit/` requires the full suite green before
   commit**: `.venv/bin/python -m pytest` (644 tests at the time of writing —
   if your count differs, update the documented count and record why).

## 5. Native engine rules (`native/` — the C++20 core, issue #1)

The audit (`docs/issues.md` / issue #1) specifies a phased rewrite into a
C++20 systems engine. Rules for that work:

1. **All native code lives under `native/`** — never mixed into
   `src/pdftoolkit/` (Python) or into a root-level `src/`/`include/` pair
   that collides with the Python package layout. The root `CMakeLists.txt`
   is a thin shim that `add_subdirectory(native)`s. Rationale: ADR-0002.
2. **C++20, zero warnings.** `-Wall -Wextra -Wpedantic -Wconversion` (plus
   MSVC `/W4`) must pass clean; a warning is a build failure.
3. **Four build targets**, per the audit: `pdftoolkit_core` (static),
   `pdftoolkit_ffi` (shared C-ABI), `pdftoolkit_py` (Python extension),
   `pdftoolkit_cli` (native executable).
4. **No exception may cross the C-ABI boundary.** Every `extern "C"`
   function wraps its body in `try/catch (...)` mapping to integer error
   codes (audit Task 7.1).
5. **Audit paths map into `native/`**: the audit's `include/pdftoolkit/X`
   and `src/X` become `native/include/pdftoolkit/X` and `native/src/X`.
   Follow the audit's module layout exactly (memory/, parser/, codec/,
   font/, layout/, index/, search/, geometry/, ops/, runtime/, ffi/).
6. **The Python engine stays authoritative** until the native core reaches
   feature parity and the audit's performance gates pass. Native work is
   additive scaffolding until then — do not delete or stub Python
   capabilities in favor of unfinished native ones.
7. **Every performance claim needs a recorded benchmark** (Google
   Benchmark, task 0.2 — policy ADR-0005): the pipeline is
   `native/benchmarks/` (behind `PDTK_ENABLE_BENCHMARKS`, default ON;
   benchmarks exist only when the components they measure exist), the
   runner is `scripts/run_perf.sh`, and results are recorded under
   `docs/benchmarks/` — a number not recorded there, with its command and
   environment, may not be cited anywhere (docs, registries, commits).

## 6. Commit message format

```
<area>: <imperative summary of the completed task>

Task: <task id, e.g. issue-1/task-0.1 — plus GitHub issue ref #N>

What changed:
- ...

Why:
- ...

What was tested / verified:
- <command> → <result>

What remains:
- ...

Related: <problem-registry ids, ADR numbers, previous commits>
```

`<area>` is one of: `engine` (Python engine), `native` (C++ core),
`docs`, `build`, `ci`, `tests`, `repo`.

## 7. Environment constraints (verified 2026-10-09)

* System Python is **externally managed (PEP 668)** — always use the repo
  virtualenv `.venv/` (created with `python3 -m venv .venv`; install with
  `.venv/bin/pip install -e ".[dev,http]"`).
* Toolchain present in the reference environment: GCC 14.2.0 (full C++20),
  CMake 4.4.4 and Ninja 1.13.2 **installed via pip** (`.venv/bin/cmake`,
  `.venv/bin/ninja`) — cmake/ninja are NOT system-installed.
* **No clang locally** → MemorySanitizer presets cannot be verified locally
  (MSan requires clang); they are CI-only. ASan/UBSan/TSan work with GCC.
* **No perf locally** → the `perf stat` stage of `scripts/run_perf.sh`
  prints `SKIPPED (<reason>)` and exits 0 (U-010). A missing perf is never
  a benchmark failure; hosted CI runners are expected to behave the same.
* The full Python suite needs the `http` extra installed or
  `tests/adapters/test_http.py` fails at *collection* (it does not skip) —
  an environment pitfall recorded in the problem registry.
* NumPy 2.2.4 is available (needed later for zero-copy buffer-protocol
  tests in Phase 7).

## 8. Licensing constraint

PyMuPDF is **AGPL-3.0**. The Python engine (and any service linking it)
inherits AGPL obligations — see README "License note". Native code that
does not link MuPDF is not AGPL-bound by itself; keep the licensing
boundary documented if that changes.
