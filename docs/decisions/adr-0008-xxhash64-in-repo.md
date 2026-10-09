# ADR-0008: xxHash64 is implemented in-repo, not fetched

* **Status:** Accepted (2026-10-10, task 3.1)
* **Context:** audit issue-1 prescribes xxHash64 content hashing in
  three places — task 3.1 (CMap cache dedup), task 3.3 (term hashes
  into the slab's term-hash section), and task 4.1 (corpus-level
  lexicon keys). The audit names no library, unlike Flate where it
  (misspelled but unambiguously) names libdeflate (ADR-0007).
* **Decision:** implement xxHash64 as an internal header-only
  component (`native/src/core/xxhash64.hpp`, namespace
  `pdftoolkit::hash`, streaming class + one-shot), never installed,
  never included from public headers — the same contract as
  `scan_util.hpp`.
* **Rationale:**
  * The algorithm is ~150 lines, spec-stable since 2016 (the xxHash
    specification is public domain), with zero build-system surface.
    A third FetchContent dependency (plus the ADR-0007-style offline
    stub treatment) would cost more than the algorithm itself.
  * The offline guarantee (ADR-0003) stays airtight by construction:
    no new configure-time network dependency.
  * Known-answer tests pin the implementation to the canonical C
    library: reference vectors were generated via the Python `xxhash`
    wheel (which bundles the canonical sources) and are embedded in
    `native/tests/test_cmap.cpp`, covering every stripe/tail boundary
    (lengths 31/32/33/63/64/65), incremental-vs-one-shot equivalence,
    and seed domain separation. This caught a real spec misreading
    before it shipped (the XXH64 stripe is four 8-byte lanes, not
    XXH32's eight 4-byte lanes round-robin).
* **Consequences:**
  * Future hash consumers (3.3, 4.1) use `pdftoolkit::hash` and must
    pass a distinct seed when the same bytes must not produce the same
    key across roles (domain separation; task 3.1 seeds raw vs Flate
    CMap payloads differently so payload classes can never
    collide-merge).
  * If xxHash requirements ever grow beyond the 64-bit spec (e.g.
    XXH3's 128-bit variant), revisit and either extend in-repo against
    the spec's reference vectors or fetch the canonical library under
    an ADR-0007-style policy — do not grow a private variant.
  * Any edit to `xxhash64.hpp` must keep the embedded reference
    vectors passing; the vectors are the proof nothing drifted.
