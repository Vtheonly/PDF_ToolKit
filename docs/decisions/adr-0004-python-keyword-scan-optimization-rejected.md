# ADR-0004 — Reject the Python-level combined-keyword scan optimization

* **Status:** accepted (2026-10-09)
* **Supersedes:** none
* **Context:** Problem P-003 (audit defect 3) shows `count_keywords` in
  `core/keywords.py` performs K unindexed regex scans per page (K keywords
  × P pages). The obvious Python-level idea is to merge all keywords into
  a single alternation pattern and scan each page once, cutting K×P scans
  to P. This ADR records why that idea is **semantically impossible** and
  must not be attempted again.

* **Decision:** Do not implement a combined-regex (or lookahead-based)
  Python-level scan in `count_keywords`. The function's contract is
  per-keyword **non-overlapping** match counts along the two documented
  axes (`case_sensitive`, `whole_words`). The only accepted fix for P-003
  is the native inverted index + Block-Max WAND (issue #1 Tasks 4.1/4.2).

* **Analysis (the two failure modes):**

  1. *Leftmost-first alternation.* Python `re` alternation commits to the
     first alternative that matches at a position; it does not retry other
     alternatives for counting purposes. Example (substring mode,
     `whole_words=False`): keywords `foo`, `foobar`; text `foobar`.
     Current behaviour: `foo=1` and `foobar=1` (two independent scans).
     Combined pattern `foo|foobar`: matches `foo` at position 0, consumes
     it, resumes at position 3 → `foobar` is never counted. Counts change.
  2. *Lookahead scanning.* To let every keyword match at every position,
     one would scan `(?=(foo|foobar))` via `finditer` — but lookahead
     matches **overlap**, while the contract requires **non-overlapping**
     counts per keyword. Example: keyword `aa`, text `aaa`:
     `findall("aa", "aaa") == 1` (non-overlapping), but the lookahead
     approach yields positions 0 and 1 → count 2. Counts change.

  Preserving exact semantics therefore requires per-keyword scans in the
  general case; any "single-pass" variant changes user-visible counts,
  breaking the 644-test suite's locked expectations (tests
  `test_keywords*.py`, `test_search_behaviour.py`) and the documented
  MatchResult contract.

* **Additional verified fact:** `re.compile` calls in `keyword_pattern`
  are effectively free — CPython's `re` module caches compiled patterns
  internally (512-entry cache), so compilation is not the bottleneck;
  scanning is. There is nothing meaningful to win at the Python level.

* **Consequences:**
  * P-003's fix is firmly architectural (native core); no Python drift.
  * Any future proposal to alter `count_keywords` semantics must first
    supersede this ADR and update the frozen-contracts list in
    `docs/architecture/python-engine.md`.
