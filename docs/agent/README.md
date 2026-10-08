# Agent Documentation — Index

This folder is the **agent guidance and continuity system** for PDF_ToolKit.
It exists so that any future agent can understand, without re-discovery:

* what has already been done and verified,
* what decisions are locked in and why,
* what remains open, blocked or unknown,
* and how to work on this repository without breaking it.

## Reading order for a new agent

1. `../../AGENTS.md` — permanent rules (the entry point).
2. `workflows.md` — the mandatory task lifecycle and commit/push discipline.
3. `dos-and-donts.md` — hard rules distilled from real mistakes.
4. `../../docs/recovery/task-registry.md` — what is done / in progress / blocked.
5. `../../docs/recovery/problem-registry.md` — known problems and debt.
6. `../../docs/recovery/unknowns.md` — open questions and assumptions.
7. `../../docs/architecture/` — how the system is actually structured.
8. `../../docs/decisions/` — ADRs; never contradict one without a new ADR.
9. `../../docs/recovery/change-log.md` — evolution over time, newest first.

## Maintenance rules

* These documents are **living state**, not historical essays. Update them
  in the same commit as the work they describe.
* Every claim of "done" or "verified" must carry a reproducible command.
* Prefer deleting or archiving stale entries over leaving contradictory ones.
