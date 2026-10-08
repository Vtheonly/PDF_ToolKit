# Architecture Documentation

Current-state and target-state architecture knowledge for future agents.
`../../ARCHITECTURE.md` (repo root) is the original v2.0.0 design document;
these pages extend it with verified, session-maintained detail.

| Page | Contents |
|------|----------|
| `python-engine.md` | Verified map of the shipping Python engine: layers, dependency edges, envelope flow, registry flow, test anatomy |
| `native-tree.md` | Target native C++20 tree (issue #1): layout, build targets, coexistence & isolation model, audit-path mapping |

Rules:

* Update these pages whenever a structural fact changes (new layer, new
  dependency edge, new build target).
* Every diagram must reflect **verified** reality — no aspirational boxes;
  planned items are marked `(planned, issue-1/task-X.Y)`.
