# Change Log — Significant Discoveries & Completed Changes

Newest first. One entry per meaningful change or discovery, with commit
reference and evidence pointers. This is evolution history, not a changelog
for end users.

---

## 2026-10-09 — Session 1 (issue #1 kickoff: agent infrastructure + continuity system)

* **Repository state verified (T-000, `not_applicable`):** the repo has
  exactly one branch — `main` — and zero pull requests
  (`git ls-remote --heads origin`; GitHub API `/branches`, `/pulls`).
  The requested "merge all branches into main and delete the others" end
  state already held; no merge or deletion was performed, so no history
  was lost or rewritten.
* **AGENTS.md created (T-001, commit `932cf07`)** as the first iteration
  of permanent agent instructions, plus `docs/agent/` guidance folder
  (index, workflows, DOs/DON'Ts).
* **Continuity documentation system created (T-002, this commit):**
  `docs/recovery/` (task registry, problem registry, unknowns, this change
  log), `docs/architecture/` (python-engine map, native-tree plan),
  `docs/decisions/` (ADR-0001..0004).
* **Environment verified and pinned in docs:** GCC 14.2.0 (C++20-capable),
  CMake 4.4.4 + Ninja 1.13.2 via pip venv (not system-installed), Python
  3.12.14, PyMuPDF 1.28.2, pytest 9.1.1, NumPy 2.2.4; system Python is
  PEP-668-managed (`.venv/` mandatory). Full extras installed:
  `pip install -e ".[dev,http]"`.
* **Baseline established:** `644 passed, 1 warning in 5.53s`
  (`.venv/bin/python -m pytest`) and `644 tests collected` — the Python
  engine is green at the session start SHA `297819d`.
* **Discovery P-006:** README/ARCHITECTURE claimed 646 tests; actual 644.
  Counts corrected (T-003) with the measuring command recorded.
* **Discovery P-007:** `tests/adapters/test_http.py` errors at collection
  (instead of skipping) when the `http` extra is absent — the module-level
  `fastapi` import defeats its own `skipif` guard. Reproduced live; fix
  filed as T-004 (pending).
* **Discovery P-003-constraint / ADR-0004:** the tempting Python-level
  "single-pass combined-keyword regex" optimization of `count_keywords`
  is semantically impossible (leftmost-first alternation and lookahead
  scanning both break documented count semantics). Documented so no future
  agent burns time on it.
* **ADRs recorded:** 0001 single-branch policy; 0002 native-tree isolation
  under `native/`; 0003 Phase-0 offline-build minimal-dependency rule;
  0004 rejection of the Python-level scan optimization.
