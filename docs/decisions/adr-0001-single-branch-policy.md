# ADR-0001 — Single-branch (`main` only) repository policy

* **Status:** accepted (2026-10-09)
* **Supersedes:** none
* **Context:** The repository owner directed: "merge all the branches into
  one single main branch and remove the others but main". Verification
  (2026-10-09, `git ls-remote --heads origin` + GitHub API `/branches` and
  `/pulls` — state all branches): the repository already contained exactly
  one branch (`main`) and zero pull requests. The project is a
  single-maintainer, agent-assisted codebase with a linear history
  (9 commits at verification time).
* **Decision:** Adopt a permanent single-branch policy:
  * All work is committed directly to `main`.
  * No feature branches, no release branches, no PR queues.
  * Push after every commit; never accumulate unpushed work.
  * No history rewrites, no force pushes.
  * `main` must be green (Python suite + native build) at every push.
* **Alternatives considered:**
  * *Feature branches + PRs* — rejected: for an agent-driven workflow,
    PR overhead adds latency with no reviewer benefit; the owner asked for
    the exact opposite (everything on main, merged after each commit).
  * *Git-flow (develop/release)* — rejected: far too heavy for this
    project size.
* **Consequences:**
  * Commit granularity must be disciplined: one logical change per commit
    with detailed messages (AGENTS.md §6) — the commit log is the only
    review surface.
  * Risk of broken `main` is mitigated by the mandatory green-before-push
    rule and per-commit verification evidence.
  * If the project ever gains external contributors, this ADR must be
    revisited (a PR flow would then supersede it).
