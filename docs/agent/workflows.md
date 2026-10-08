# Agent Workflows — Task Lifecycle, Commits, Pushes

## 1. The mandatory task lifecycle

Every task — whether it comes from GitHub issue #1's audit plan, a bug
report, or a spontaneous discovery — goes through the same seven steps.
A task is not "done" until **all seven** are complete.

```
┌────────────┐   ┌────────────┐   ┌────────────┐   ┌────────────┐
│ 1. inspect │──▶│ 2. plan    │──▶│ 3. implement│─▶│ 4. verify  │
└────────────┘   └────────────┘   └────────────┘   └─────┬──────┘
                                                          │
┌────────────┐   ┌────────────┐   ┌────────────┐          │
│ 7. commit  │◀──│ 6. update  │◀──│ 5. update  │◀─────────┘
│   + push   │   │ registries │   │ docs       │
└────────────┘   └────────────┘   └────────────┘
```

### Step 1 — Inspect (before making changes)

* Read `AGENTS.md` §4/§5 rules for the area you will touch.
* Read the existing implementation and **map its dependencies** (who calls
  it, what it calls, what tests lock its behaviour).
* Read the task registry entry for the task, if one exists.
* Reuse and extend existing logic. If you are tempted to write a second
  implementation of something that exists, stop and re-read
  `dos-and-donts.md`.

### Step 2 — Plan

* Confirm the task's acceptance criteria (from the audit or issue).
* Decide the smallest change that satisfies them.
* If a decision is irreversible or constrains the future, write an ADR
  (`docs/decisions/adr-NNNN-*.md`) **before** implementing.

### Step 3 — Implement

* One logical change; no drive-by refactors outside the task's scope.
* Follow layering rules (`AGENTS.md` §4/§5).

### Step 4 — Verify (evidence or it did not happen)

* Python engine: `.venv/bin/python -m pytest` (full suite, must be green).
* Native code: configure + build with zero warnings, then `ctest`.
* Record the **exact command and its output summary** — this is the
  evidence that goes into the task registry entry.
* If verification is impossible in the current environment, the task stays
  `blocked` with a `docs/recovery/unknowns.md` entry — never `done`.

### Step 5 — Update docs

For every task record: what was wrong, why it happened, what changed, what
was verified, what remains unresolved. Files affected: the relevant
`docs/architecture/*.md`, `docs/decisions/adr-*`, README/ARCHITECTURE when
user-facing facts changed (test counts, layout, commands).

### Step 6 — Update registries

* `docs/recovery/task-registry.md` — mark status + evidence.
* `docs/recovery/problem-registry.md` — new problems found; resolved ones
  get `status: resolved` with the fix commit.
* `docs/recovery/unknowns.md` — anything you could not verify.
* `docs/recovery/change-log.md` — append the entry (newest first).

### Step 7 — Commit and push (immediately)

* Commit format: see `AGENTS.md` §6.
* Single-branch policy: commit to `main`, `git push` right away.
* Never start the next task with unpushed commits.

## 2. Discovery protocol (new knowledge)

If during implementation you discover ANY of the following, it must be
persisted **in the same commit**:

| Discovery | Where it goes |
|---|---|
| Bug / regression | `problem-registry.md` (new entry, `status: open`) |
| Architectural problem / drift risk | `problem-registry.md` + possibly an ADR |
| Undocumented constraint / dependency | `docs/architecture/*.md` + `AGENTS.md` if it is a standing rule |
| Unverifiable assumption | `unknowns.md` |
| Root cause of a past mystery | problem entry gets a `Root cause:` line |
| Decision (incl. rejected alternatives) | new ADR in `docs/decisions/` |

Test: *could the next agent repeat your mistake because this knowledge was
not written down?* If yes, write it down.

## 3. Session bootstrap checklist

```bash
cd <repo>
git status                       # clean, on main
git pull --ff-only               # newest state
source .venv/bin/activate        # or use .venv/bin/... directly
python -m pytest                 # baseline must be green (644 tests)
cmake --version                  # via .venv if not system-installed
```

Then read the task registry's **Next tasks** section and pick the top
in-scope task. Do not invent scope.

## 4. Session close-out checklist

* All commits pushed; `git status` clean.
* Task registry reflects reality (no aspirational statuses).
* Change-log entry for the session appended.
* Final report covers: tasks completed, problems fixed, tests performed,
  documentation updated, new discoveries, commits created, problems
  remaining, blocked tasks, what to tackle next.
