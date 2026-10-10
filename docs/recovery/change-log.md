# Change Log — Significant Discoveries & Completed Changes

Newest first. One entry per meaningful change or discovery, with commit
reference and evidence pointers. This is evolution history, not a changelog
for end users.

---

## 2026-10-10 — Session 6: issue-1/task-3.2 done (content stream operator state machine)

* **OperatorEvaluator landed (audit task 3.2):**
  `native/include/pdftoolkit/layout/evaluator.hpp` +
  `native/src/layout/evaluator.cpp` — the layout/ module's first
  component. Push-down, zero-heap, noexcept state machine over the
  2.3 ZeroCopyLexer: full text state (Tc/Tw/Tz/TL/Tf/Tr/Ts, q/Q-
  saveable), positioning (BT/ET, Td/TD, Tm, T*), showing (Tj, ', ",
  TJ with kerning numbers), graphics state (q/Q + cm — CTM is
  otherwise unreachable). Glyph advances update Tm but NEVER Tlm
  (the line-matrix subtlety: Td(0,0) rewinds to the line start);
  a TJ number's displacement is -(Tj/1000)*Tfs*Th; Tw only on the
  single-byte code 32.
* **ADR-0009 (precision model, load-bearing):** the audit mandates
  float32 `std::array<float,6>` matrices AND a +/-0.001 pt gate —
  these CONFLICT: a simulated float32 displacement accumulator drifts
  **1.309e-03 pt over a 400-glyph line** (the golden generator's
  drift probe), past the gate. Decision: public Mat6 stays float32,
  internal carry (displacement D, Tlm, CTM, the 14-flop per-glyph
  Trm closed form against P = Tlm x CTM) is double. Acrobat itself
  computes in 16.16 fixed point (~1.5e-5 pt) — the gate never
  implied float32 arithmetic sufficed.
* **U-015 (the Acrobat-reference interpretation):** no Acrobat output
  exists in-repo; the gate's reference is the INDEPENDENT Python
  model `scripts/gen_evaluator_golden.py` (f32 operand parse -> f64
  algebra, fused emission+evaluation, seed 0x3D2E1F, committed for
  reproducibility). 12 scenarios x 605 golden glyphs — every glyph's
  6-field Trm + advance and every final matrix within 1e-3 pt.
* **TJ arrays without buffering:** an array's byte RANGE is recorded
  and re-lexed only when TJ follows — `[3 2] 0 d` (dash patterns are
  everywhere in real streams) can never move the pen; keywords
  inside dicts (R/true/false/null) and arrays are members, never
  operators. Both test-locked; 16 bytes of recorder state instead of
  a multi-KB element buffer.
* **Two seams for Phase 3's remainder:** GlyphSink (per-glyph code /
  font / Trm / advance — task 3.3's input) and FontMetricsResolver
  (code width + w0 per font resource — P-023's docking point, second
  after CMapCache::intern_stream_object; default 1-byte/1000 = ISO
  /DW until the resolver lands, P-025).
* **P-023 SCHEDULED:** the object-graph resolver must land BEFORE or
  WITH task 3.3 (next-tasks item 1; P-025 records the decision).
* **Test-drive findings (test-side, instructive):** 3 dangling
  string_view reads (font views die with the content buffer — the
  documented zero-copy lifetime contract; the capture sink now copies
  font names), 2 expectation errors that were REAL semantics
  learnings (cm pre-multiplication order: a later cm's translation is
  scaled by the earlier one; TD overwrites TL), 1 arithmetic slip
  (Tw difference includes the width delta), and float32 advance
  fields need 1e-6 tolerances. The implementation itself needed no
  post-test fixes on the golden gate — the independent-model method
  worked as designed.
* **Matrix after the change:** release 14/14 zero-warning (ctest grew
  from 13), debug 14/14, asan+ubsan 13/13, tsan 13/13, offline
  14/14 (no _deps), Python 644. Benchmark suite is now 7 binaries
  (pdtk_bench_evaluator added; CI's benchmark step auto-discovers it
  via run_perf.sh). Recording: `docs/benchmarks/
  2026-10-10-phase3-evaluator-baseline.md` — text-heavy 150.0 M
  glyphs/s (6.67 ns/glyph) vs same-run control 4620.1 MB/s.

---

## 2026-10-10 — Session 5: issue-1/task-3.1 done (CMap cache & Unicode resolver — Phase 3 opened)

* **CMapTable + CMapCache landed (audit task 3.1):**
  `native/include/pdftoolkit/font/cmap.hpp` +
  `native/src/font/cmap.cpp`. Lexer-driven /ToUnicode parse (bfchar,
  both bfrange forms, codespace), UTF-16BE destinations with surrogate
  pairs, build-time ligature expansion (verified table — P-024 scope
  line), positional-pair malformed-entry alignment, hostile-range
  refusal, O(1) frozen direct-index tables, xxHash64 payload dedup
  with byte-exact confirmation, owned Flate codec + rewound scratch
  arena. Evidence: task-registry 3.1 row; pdtk_test_cmap 34 cases;
  recording `docs/benchmarks/2026-10-10-phase3-cmap-baseline.md`
  (lookup 700.6 M/s = 1.43 ns; intern hit 938.6 k/s vs miss 59.4 k/s).
* **xxHash64 implemented in-repo (ADR-0008):**
  `native/src/core/xxhash64.hpp` — streaming + one-shot, internal
  header. Reference vectors generated from the canonical C library
  (Python `xxhash` wheel) and embedded in test_cmap.cpp; the vectors
  CAUGHT A REAL SPEC MISREADING during development: the XXH64 stripe
  is four 8-byte lanes (acc1<-[0..8) .. acc4<-[24..32)), not XXH32's
  eight 4-byte lanes round-robin — short inputs (< 32 B) passed while
  every striped input hashed wrong. Known-answer tests are how spec
  algorithms land here, not eyeballing.
* **stream_util.hpp extracted from xref.cpp (extend-not-fork):** the
  shared stream-object anatomy (`N G obj` header, direct-integer
  /Length refusal P-021, /Filter classification incl. /Fl,
  /DecodeParms predictor detection P-020, `stream`+EOL payload
  location) now lives in `native/src/parser/stream_util.hpp`; xref.cpp
  calls the same primitives (its 23-case suite + 2,200-doc corpus
  re-verified after the refactor — the same extraction discipline that
  caught 2 walker bugs in task 2.2).
* **Three implementation bugs the test-drive caught before ship:**
  (1) the block-parser token loop was missing its update clause (first
  token processed forever — caught as a hang); (2) UTF-16BE units
  were built from 2 hex digits instead of 4 (every destination decoded
  as doubled garbage codepoints); (3) a malformed bfchar SOURCE did
  not consume its positional destination, cascading misalignment that
  manufactured wrong mappings from corrupt input — pairs/triples now
  die whole. Plus a bfrange array-cursor initialization bug (bound
  destroyed by `lo - 1`).
* **P-023 filed (planning defect):** the audit never assigns the
  object-graph resolver (following /Font /ToUnicode N 0 R through
  references and /ObjStm) to any task; task 3.1's
  `intern_stream_object(full object bytes)` is the seam a future
  resolver plugs into.
* **Matrix after the change:** release 13/13 zero-warning (ctest grew
  from 12), debug 13/13, asan+ubsan 12/12, tsan 12/12 (validates the
  cache mutex), offline 13/13 (no _deps), Python 644. Benchmark suite
  is now 6 binaries (pdtk_bench_cmap added; CI's benchmark step picks
  it up via run_perf.sh auto-discovery).

---

## 2026-10-09 — Session 4: issue-1/task-2.4 done (libdeflate Flate codec — Phase 2 complete)

* **FlateDecompressor landed (audit task 2.4):**
  `native/include/pdftoolkit/codec/flate.hpp` +
  `native/src/codec/flate.cpp` — the audit's `src/codec/flate.cpp` slot
  under the ADR-0002 path mapping. zlib-wrapper decompression (the PDF
  /FlateDecode container, RFC 1950) via **libdeflate v1.26**; the
  audit's "libdeflater" name does not exist as a C library (P-019) —
  integration policy, offline escape hatch and pin discipline in
  **ADR-0007** (first RUNTIME dependency of pdftoolkit_core; the
  ADR-0005 option pattern applied: `PDTK_ENABLE_FLATE`, default ON;
  OFF = honest stub returning `FlateStatus::Unavailable`, verified
  offline with no `_deps`).
* **Arena staging per the audit's step 3, enabled by a minimal
  BumpArena extension:** `rewind_to(mark)` (speculative sub-allocation,
  only-rewinds semantics, test-locked in pdtk_test_arena). The codec
  speculates a chunk (8x compressed, or the caller's trusted metadata
  hint), doubles on INSUFFICIENT_SPACE, and REWINDS between attempts —
  consumption stays at the final chunk, never the sum. When a
  speculative chunk exceeds the arena's remaining capacity the codec
  retries with exactly that capacity before refusing.
* **Bomb defenses exactly as prescribed (step 4):** expansion ratio
  (default 128x), absolute output budget (default 64 MiB), and the
  metadata size hint as a hard bound when present. A refused stream
  leaves the arena exactly at its mark — no staging bytes survive a
  refusal (contract change forced by the test suite, which caught dead
  chunks on every refusal path).
* **P-017a RESOLVED: Flate xref streams are first-class.**
  `parse_xref_stream` now classifies `/Filter` (exactly one
  FlateDecode — spelled out or legacy `/Fl` — is first-class; anything
  else degrades), refuses indirect `/Length` (P-021 — the landed 2.2
  code half-parsed `999 0 R` as the byte count 999; harmless for raw
  rows, wrong as a compressed extent), decompresses into a lazily
  created 64 MiB scratch arena (rewound section-to-section) and feeds
  the SAME row decoder (`decode_rows`, extracted from the inline loop).
  Predictor-bearing `/DecodeParms` deliberately degrade (P-020 — a
  silently mis-decoded index is worse than a flagged scan).
* **Two benchmark-driven codec fixes, measured:** (1) a 4x first guess
  wasted one full aborted attempt on EVERY ~5x-ratio content stream
  (measured 537-625 MB/s; efficiency ~55%) — 8x single-shots the
  common 3-6x PDF ratios, lifting content throughput to ~1035-1089
  MB/s; (2) the incompressible workload's 8x guess overshot the 32 MiB
  benchmark arena with a 16 MiB output — the fit-to-arena retry. Both
  documented in the recording.
* **GATE MET (>= 800 MB/s/core):** content mix level 6 (the producer
  default) 1034.7 MB/s mean, P50/P90/P99 1035.0/1039.7/1040.7, CV
  1.6%; level 1/9 within 1036-1089 MB/s; repetitive 4511.5 MB/s;
  incompressible 7533.0 MB/s; same-run control traversal 4538.4 MB/s
  (content ratio ~23%). Recording:
  `docs/benchmarks/2026-10-09-phase2-flate-baseline.md`.
* **pdtk_bench_flate landed** (component-exists map — exists only when
  `PDTK_HAVE_FLATE`: a stub must never produce numbers). Suite now five
  binaries; the full pipeline run (CI-equivalent command) verified.
* **Tests:** pdtk_test_flate 19 cases (round-trips 1 B-1 MiB, chunk
  growth/reuse, every bomb defense, corrupt/truncated/gzip-container
  mapping, arena integration, move semantics, 200 hostile buffers x2
  determinism; the offline stub has its own case). test_xref grew to
  23 cases + the corpus to **2,200 documents** (200 REAL
  zlib-compressed Flate xref streams — verified first-class on default
  builds; the same block degrades deterministically when the codec is
  compiled out). test_arena +1 (rewind_to).
* **Verification matrix (2026-10-09):** release zero-warning ctest
  **12/12**, debug 12/12, asan+ubsan 11/11 (libdeflate instrumented
  too), tsan 11/11, offline (`PDTK_ENABLE_FLATE=OFF` +
  `PDTK_ENABLE_BENCHMARKS=OFF`) 12/12 with **no `_deps`**, Python 644.
  All benchmarks 5/5 binaries through `scripts/run_perf.sh --json`.
* **P-022 — CI caught what the local matrix could not:** the first
  push's native jobs failed on the clean runner (`libdeflate.h: No such
  file or directory` in pdtk_test_xref) while every local check was
  green — the reference sandbox carries a SYSTEM libdeflate 1.23-dev
  whose `/usr/include` header silently satisfied the include, with
  symbols still resolving from the pinned v1.26 archive (static-lib
  PRIVATE propagation). Fixed by linking `libdeflate_static` to
  pdtk_test_xref; registry entry documents the trap (any TU including
  a FetchContent'd header must link that target explicitly).
* **Phase 2 is complete** (2.1 trailer, 2.2 xref, 2.3 lexer, 2.4
  Flate). Next per the registry: task-3.1 (CMap cache & Unicode
  resolver — depends on 2.3+2.4, both done).

---

## 2026-10-09 — Session 3 (cont.): issue-1/task-2.3 done (zero-copy lexer + bench_lexer)

* **ZeroCopyLexer landed (audit task 2.3):**
  `native/include/pdftoolkit/parser/lexer.hpp` +
  `native/src/parser/lexer.cpp`. Emits the audit's `PdfToken` verbatim
  (plus `EndOfFile` — the audit's enum list has no exhaustion state, an
  underspecification noted in the registry). Zero-copy `string_view`
  values into the caller's buffer; raw semantics documented (strings
  keep parentheses/escapes, names keep `#`-escapes — decoding needs a
  copy and is deferred to consumers).
* **Branchless 256-entry LUT exactly as prescribed:** `alignas(64)`
  whitespace table driving separator/comment skipping; the
  classification rules stay single-sourced in scan_util.hpp (the LUT
  materializes them for the hot loop).
* **Number handling:** locale-independent `std::from_chars` (int64 with
  PDF-legal leading `+` that from_chars itself rejects for integers —
  skipped manually; saturating overflow), int/real classification by
  syntax scan, raw values always preserved; garbage suffixes tolerated.
* **Tolerant skips are ITERATIVE — a design review catch:** the first
  formulation recursed for stray `>`/`{`/`}` bytes; a hostile
  megabyte-long brace run would have overflowed the stack. Regression-
  locked (`junk_runs_do_not_recurse_or_hang`, 1 MB of `{`).
* **Stream contract:** `stream`/`endstream` arrive as the audit's
  StreamStart/StreamEnd; after StreamStart the CALLER owns payload
  positioning (offset() + skip-one-EOL; resume by constructing a fresh
  lexer over the suffix) — the lexer never scans binary payload for
  `endstream`. Test-locked.
* **`pdtk_bench_lexer` landed** (the audit's `bench_lexer` slot,
  component-exists map): content mix 350.7 MB/s mean (67.7 M tokens/s,
  P50/P90/P99 350.8/352.6/353.2 MB/s, CV 0.6 %), string-heavy 833.3
  MB/s, control traversal 4.586 GB/s — **the 2.5 GB/s gate is honestly
  UNMET (P-018)**: at ~5 bytes/token the gate needs ~6 cycles/token,
  which is SIMD-classification territory (task 4.3's technology), not a
  scalar token-at-a-time lexer. Recording (with the same-run control
  and the full analysis):
  `docs/benchmarks/2026-10-09-phase2-lexer-baseline.md`.
* **U-012 sharpened:** the sandbox ceiling is RUN-dependent, not just
  environment-dependent — the same-day control moved 2.40–2.85 →
  4.50–4.78 GB/s (~1.7×). Rule recorded: never ratio a component
  against a control from an earlier session; re-measure both in one
  run.
* **Docs hygiene:** the benchmarks ledger index was missing the
  session-2 mmap and slab recordings — rows added.
* **Verification matrix (all 2026-10-09):** release zero-warning + ctest
  **11/11** (the new `lexer` ctest grows the suite from 10); debug
  11/11; asan+ubsan 10/10 (incl. 200 hostile random buffers, re-lexed
  deterministically); tsan 10/10; offline (no FetchContent) 11/11;
  Python 644 passed.
  **Live CI on the task-2.3 commit (f546fbe): all 5 jobs green** — run
  37852065321; the benchmark step now runs all four pdtk_bench_*
  binaries on the hosted runner.

---

## 2026-10-09 — Session 3 (cont.): issue-1/task-2.2 done (dual-mode XRef resolver)

* **XRefIndex landed (audit task 2.2):**
  `native/include/pdftoolkit/parser/xref.hpp` +
  `native/src/parser/xref.cpp`. One entry point —
  `XRefIndex::from_document(bytes, tail_window)` — composes 2.1's
  `locate_startxref`, resolves the cross-reference structure it names
  and builds the contiguous id-indexed table. Classic plaintext tables
  (multi-subsection, tolerant entry parsing), `/Prev` chains with
  newest-entry-wins merge semantics (a newer FREE entry sticks — the
  object was deleted by that update), cycle-guarded and depth-capped,
  XRef streams with variable-width `/W` fields (w1=0 default-type
  included), `/Index` ranges, type 0/1/2 rows, big-endian fields up to
  8 bytes, `/Length` consistency validation, hybrid `/XRefStm`
  companions (classic-first precedence), and the audit's emergency
  linear scan over `N G obj` headers (later occurrence wins — matches
  incremental-update restatements; flagged via `from_linear_scan()`).
* **Flate honesty (P-017a):** most real PDF 1.5+ xref streams are
  FlateDecode-filtered and unparseable until task 2.4's decompressor.
  A Flate newest section degrades to the linear scan — a genuine
  partial index (all stand-alone objects found; objects compressed
  inside ObjStms are invisible to any header scan) — instead of a
  failure or, worse, a faked success.
* **Hardening:** a 10-million-object cap refuses subsection/`/Index`
  ids beyond it (a hostile "4294967290 5" subsection header would
  otherwise materialize a ~100 GB vector — refused, not OOM'd;
  test-locked), and chain walking is capped at 4096 links.
* **scan_util.hpp extracted (extend, never fork):** trailer.cpp's
  character classes, string/dict skippers and number parsers moved to
  the internal header `native/src/parser/scan_util.hpp`, which xref.cpp
  builds on. Two real bugs were found and fixed in the shared walker
  along the way, both caught by suites: (1) the last dictionary entry
  was silently dropped when the walked range excludes the closing `>>`
  (body-only ranges never hit the depth-0 emit — fixed with an
  emit-on-range-end); (2) a name VALUE (`/Type /XRef`) was mistaken
  for the next KEY, eating the value — fixed by adding `skip_value`,
  a syntactic value skipper (names, strings, arrays, nested dicts)
  that the walker applies after each key. Task 2.3's lexer should
  absorb these primitives when it lands.
* **Acceptance criterion met:** "correctly parses both classical tables
  and compressed streams, verified against a test corpus containing
  corrupt and linearized PDFs" — a 2,000-document deterministic corpus
  spanning classic / incremental-update / linearized-style / stream /
  hybrid / corrupt / Flate documents, every expected entry (kind,
  offset, generation, objstm fields) asserted; 18/18 test cases
  including the mmap composition proof. The `vector<uint64_t>`
  underspecification is P-017b.
* **Verification matrix (all 2026-10-09):** release zero-warning + ctest
  **10/10** (the new `xref` ctest grows the suite from 9); debug 10/10;
  asan+ubsan 9/9; tsan 9/9; offline (no FetchContent) 10/10; Python 644
  passed.
  **Live CI on the task-2.2 commit (59e6494): all 5 jobs green** — run
  37850609938 (pytest py3.9/3.12, native release + benchmarks, asan,
  tsan).

---

## 2026-10-09 — Session 3: issue-1/task-2.1 done (backward startxref & trailer scanner)

* **Phase 2 opened (audit task 2.1):**
  `native/include/pdftoolkit/parser/trailer.hpp` +
  `native/src/parser/trailer.cpp` — `locate_startxref(span, tail_window =
  1024)` returning `TrailerInfo` (xref offset, keyword position, zero-copy
  classic-trailer dictionary view, `/Root` reference, `/Prev` offset).
  Zero-copy by construction: every view points into the caller's buffer
  (typically `MmapHandle::bytes()`); the mmap composition is test-locked
  including a pointer-identity proof that the dict view lies inside the
  mapping.
* **SIMD per the audit, but runtime-dispatched:** the backward needle
  search compiles `_mm256_cmpeq_epi8` against 's' behind a function
  target attribute and dispatches via `__builtin_cpu_supports("avx2")`,
  with a portable scalar fallback (non-x86, MSVC, pre-AVX2 hosts — and
  Debug builds, which do not get `-march=native`). The two paths are
  differential-tested over the whole acceptance corpus via the
  `detail::set_search_force_scalar` test hook (documented tests-only).
  **Implementation trap caught in review before it shipped:** the partial
  top chunk of the scan window must not be loaded with a raw 32-byte
  `_mm256_loadu` — it would read past the buffer (ASan heap overread on
  small files; SIGBUS past an mmap). Fixed with a zeroed staging buffer
  + bit mask; the 20,000 partial-chunk scans under ASan are the
  regression proof.
* **Tolerant last-valid-wins candidate loop:** keyword candidates are
  validated in descending position order; near-miss decoys
  (`startxref` glued to a regular character, `startxref\n` + non-numeric
  or overflowing junk) are skipped, so trailing junk containing decoy
  text does not defeat the scan. The 64-bit offset parse follows PDF
  integer token grammar (optional `+`, terminated by whitespace, a
  delimiter — e.g. `123%%EOF` — or end-of-buffer) with overflow
  rejection.
* **String-aware, nesting-aware trailer parsing:** the classic
  dictionary is located backward from the keyword, then re-verified
  forward with a tokenizer that skips literal strings (escapes and
  balanced parens), hex strings and nested dictionaries — `/Root` and
  `/Prev` decoys inside strings or nested dictionaries are invisible by
  construction, and the `<<`…`>>` pair must line up exactly with the
  bytes preceding the keyword or the dictionary fields stay honestly
  empty. XRef-stream-style files (no `trailer` keyword) yield an empty
  dict view by design — their stream dictionary is task 2.2's scope.
* **Acceptance criterion met:** "correct offset identification across
  10,000 PDF test files with variable whitespace and trailing junk
  bytes" — 10,000 deterministic synthetic documents (LCG-varied
  whitespace incl. `\r\n`/TAB/FF/NUL, up to 300 B of 's'-dense trailing
  junk with injected near-miss decoys, `/Root`/`/Prev` presence and
  values varied, nested-dict/string/name-suffix decoys, XRef-stream
  style every 11th document), every document asserted on offset + `/Root`
  + `/Prev` + dict view; run twice (AVX2 and forced scalar). 31/31 test
  cases total. The no-binary-fixtures interpretation is recorded as
  U-013 (with a PyMuPDF differential test as the future upgrade path).
* **Audit limitation registered (P-016):** the audit's fixed 1024-byte
  backward-scan window is a tunable, not a format limit — files with
  heavier trailing junk need a wider window. Implemented as the
  `tail_window` parameter (default 1024 per the audit), window-edge
  behaviour test-locked.
* **Verification matrix (all 2026-10-09):** release zero-warning + ctest
  **9/9** (the new `trailer` ctest grows the suite from 8); debug 9/9;
  asan+ubsan 8/8 (zero findings — includes 20k partial-chunk SIMD scans,
  the staging-buffer fix's proof); tsan 8/8; offline (no FetchContent)
  9/9; Python 644 passed.
  **Live CI on the task-2.1 commit (0f87953): all 5 jobs green** — run
  37846501597 (pytest py3.9/3.12, native release + benchmarks, asan,
  tsan); the trailer suite's 10k corpus and fork-free honesty cases run
  clean on the hosted runner.

---

## 2026-10-09 — Session 2 (cont.): issue-1/task-1.3 done (Unified Page Slab)

* **UPS binary layout landed (audit task 1.3):**
  `native/include/pdftoolkit/memory/page_slab.hpp` +
  `native/src/memory/page_slab.cpp`. One page = one contiguous
  BumpArena record: 64-byte header + SoA x/y/w/h + UTF-32 codepoints +
  sorted term hashes + opaque postings, every section 32-byte aligned
  (uniform invariant), native-endian "UPS1" format, CRC-32 over the
  slab with the crc field excluded — the CRC pins the exact byte length.
* **Audit defect corrected (P-015, same class as P-011/P-012/P-014):**
  the audit's `PageSlabHeader` (56 B of fields + `reserved[12]` = 68 B,
  `alignas(64)` → 128) could never satisfy its own
  `static_assert(sizeof == 64)`. Corrected to `reserved[8]`; every named
  field kept; `sizeof`/`alignof`/field-offset static_asserts freeze the
  format.
* **Acceptance criterion met and test-locked:** glyph counts chosen so
  raw arrays are NOT multiples of 32 force real padding — every section
  offset and every absolute coordinate-array address is 32-byte aligned
  (`coordinate_offsets_are_32_byte_aligned`, 16/16 cases incl. builder
  and view rejections, tamper detection, CRC reference vector).
* **`pdtk_bench_slab` landed** (task 0.2's `bench_slab.cpp` slot):
  coordinate extraction **1.71 × 10⁹ glyphs/s (0.585 ns/glyph, CV
  0.55 %)** — within ~2× of the environment's bare traversal ceiling,
  i.e. the layout itself costs almost nothing per glyph; build
  5.25 × 10⁶ glyphs/s (190 ns/glyph, bitwise-CRC-dominated — table/hw
  CRC optimization deliberately deferred, no gate covers build).
  Recording: `docs/benchmarks/2026-10-09-phase1-slab-baseline.md`;
  the audit's cache-hit-rate rows stay blocked by U-010 (no perf).
* **Design notes:** slabs allocate through an `alignas(64)` storage-unit
  adapter instead of extending `BumpArena` (the verified arena component
  stays untouched; alignment is the slab module's concern — revisit if a
  second consumer needs general aligned allocations). Postings are
  opaque bytes until task 4.1 defines the in-page posting format
  (honest-stub principle).
* **Verification matrix (all 2026-10-09):** release zero-warning + ctest
  **8/8**; debug 8/8; asan+ubsan 7/7; tsan 7/7; offline (no FetchContent)
  8/8; Python 644 passed.

---

## 2026-10-09 — Session 2 (cont.): issue-1/task-1.1 done (guarded MmapHandle + FFI wiring + bench)

* **Guarded memory-mapped buffer manager landed (audit task 1.1):**
  `native/include/pdftoolkit/memory/mmap.hpp` +
  `native/src/memory/mmap.cpp`. POSIX `mmap(PROT_READ, MAP_PRIVATE)` +
  `posix_madvise` (WILLNEED/RANDOM), Win32 `CreateFileA`→`MapViewOfFile`
  compile-only path, immutable-span-only exposure, rule-of-five, typed
  constructor error mapping (InvalidArgument/DocumentNotFound/
  UnreadablePdf/Internal), 128-slot guard registry. **Acceptance
  criterion met and test-locked**: truncation under a live mapping →
  `PdfToolkitException(IoTruncated)` through `guarded()`, never a crash
  (`native/tests/test_mmap.cpp`, 15 cases incl. three fork-based honesty
  deaths and capacity/recycling).
* **Design deviation from the audit text (P-014, ADR-0006):** the audit
  prescribes *throwing from the sigaction handler* — undefined
  behaviour. Implemented compliantly instead: `sigsetjmp`/`siglongjmp`
  recovery point published per-thread; the handler converts only faults
  inside registered live mappings under an active recovery point;
  everything else is re-raised with the previously-installed disposition
  (sanitizer handlers chain through). Per-slot seqlock registry keeps
  the probe async-signal-safe, allocation-free and lock-free.
* **FFI wiring (the C-ABI's promised "arrives with task 1.1"):**
  `pdftoolkit_register_document` now mmaps the file via `MmapHandle` +
  `advise(WillNeed)` (sequential ingestion hint), keeping stable ids and
  the first mapping on re-registration. Directories are now correctly
  rejected as `PDTK_ERR_UNREADABLE_PDF` (previously accepted by the
  existence check); typed exceptions translate to C-ABI statuses.
  `pdftoolkit.h` scaffolding notes updated.
* **Build-system discovery (would break any TLS-using core code):**
  linking a non-PIC static core into the shared FFI library fails on
  TLS relocations (`R_X86_64_TPOFF32`). Fixed with
  `CMAKE_POSITION_INDEPENDENT_CODE ON` for the native tree.
* **`pdtk_bench_mmap` landed** (audit task 0.2's `bench_mmap.cpp` slot,
  component-exists rule): warm 128 MiB traversal mean 2.55 GB/s
  (P50 51.37 ms / P90 65.14 ms / P99 65.88 ms, CV 14.9 %);
  map+first-touch 1.94 GB/s (CV 4.2 %); open→advise→close lifecycle
  4.59 µs. Recording: `docs/benchmarks/2026-10-09-phase1-mmap-baseline.md`.
* **Environment-ceiling discovery (U-012):** a control experiment showed
  this sandbox traverses *any* 128 MiB buffer (heap, AVX-512-vectorized
  byte-sum) at only 2.40–2.85 GB/s — the durable cross-environment
  statement is "warm mmap retains ~88–90 % of heap traversal speed".
  The audit's future gates (lexer 2.5 GB/s, scanner 4.0 GB/s) sit at or
  above this environment's raw ceiling and must be reported as
  ratios when those tasks land.
* **CI false alarm (P-013, withdrawn):** the workflow's
  `branches: [main]` filter *displayed* as `branches: ain]` — the output
  rendering layer consumed `[m` as an ANSI escape. Diagnosed as a typo,
  then disproven byte-level (`od -c`) before commit: the file was always
  correct and CI triggered on every push as configured. Recorded as a
  verification-methodology pitfall (byte-dump before believing
  "corrupted output"). Also: first CI verification of the benchmark
  step came back green (run 37826782644 on 9fb0bbc, 5/5 jobs), closing
  task-0.2's "pending live CI run" note.
* **Docs hygiene:** ADR-0005 was missing from `docs/decisions/README.md`
  index — added, together with ADR-0006. U-011 (Windows SEH
  compile-only) and U-012 filed; native-tree.md updated to the new
  7-ctest reality.
* **Verification matrix (all 2026-10-09):** release zero-warning + ctest
  7/7; debug 7/7; asan+ubsan `-fno-sanitize-recover=all` 6/6; tsan 6/6;
  offline `-DPDTK_ENABLE_BENCHMARKS=OFF` 7/7 (no `_deps`); Python 644
  passed.
  **Live CI on the task-1.1 commit (5346a2e): all 5 jobs green** — run
  37833962102; the benchmark step now executes both `pdtk_bench_arena`
  and `pdtk_bench_mmap` on the runner.

---

## 2026-10-09 — Session 2: issue-1/task-0.2 done (benchmark pipeline); task-1.2 closed

* **Benchmarking pipeline landed (audit task 0.2):** `PDTK_ENABLE_BENCHMARKS`
  option (default ON) + FetchContent `google/benchmark` **v1.9.5 pinned**
  (`native/CMakeLists.txt`); `native/benchmarks/` with `pdtk_bench_arena`;
  `scripts/run_perf.sh` (discovery, single invocation per binary, perf-stat
  stage with honest degradation); CI native-release job now runs the suite
  on every push. Sanitizer presets build with benchmarks OFF. Policy:
  **ADR-0005** (resolves U-006).
* **Task-1.2 closed with the authoritative measurement:** 1M allocs+reset =
  mean 485.48 us, P50 482.15 / P90 496.27 / P99 513.31 us (20 reps, CV
  2.4%); alloc+reset pair = 0.28 ns. Recording:
  `docs/benchmarks/2026-10-09-phase0-arena-baseline.md` (P-011 updated).
* **Discovery P-012 (audit defect, same class as P-011):** task 0.2's
  acceptance names `bench_wand` whose engine arrives in Phase 4 (task 4.2);
  the audit's "depends on 0.1" line is wrong for three of its four named
  benches. Resolution: each bench lands with the task that builds its
  component; the map is recorded in `native/benchmarks/CMakeLists.txt`.
* **Pipeline discoveries (would cost a future agent real time):**
  * running a benchmark binary twice (console + JSON) yields two
    independent measurements that drifted 0.3-4% in the mean — the runner
    now invokes each binary exactly once (ADR-0005);
  * `ReportAggregatesOnly(true)` discards the per-repetition samples the
    P50/P90/P99 computation needs — forbidden for distribution-claiming
    benchmarks (percentile method + policy: `docs/benchmarks/README.md`);
  * google/benchmark v1.9.5 JSON tags per-repetition entries
    `"run_type": "iteration"` (not `"run"`), and has **no native percentile
    output** — nearest-rank percentiles are computed from the samples;
  * `perf` is not installed in the reference environment (U-010);
    `scripts/run_perf.sh` prints `SKIPPED (<reason>)` and exits 0 —
    a missing perf must never be misread as a benchmark failure.
* **GoogleTest deferral (U-009):** ADR-0003 predicted GoogleTest would land
  with task 0.2; scope-corrected by ADR-0005 — the harness survives until a
  test need exceeds it (expected: Phase 2 corpora / task 5.1 expected-error
  tests). The stale prediction in `native/CMakeLists.txt`'s test-section
  comment was fixed in the same commit.
* **Verification matrix (all 2026-10-09, GCC 14.2):** release zero-warning
  build; offline `-DPDTK_ENABLE_BENCHMARKS=OFF` → no `_deps`, ctest 6/6;
  asan 5/5 and tsan 5/5 (both with no FetchContent — preset change
  verified); debug 6/6 (benchmarks built at -O0, not run); Python 644
  passed; ci.yml YAML + CMakePresets.json validity checked.

---

## 2026-10-09 — Session 1, continued: issue-1/task-0.1 done (native build system)

* **Native C++20 build system landed (audit task 0.1):** root `CMakeLists.txt`
  shim + `native/` tree (ADR-0002) with the four audit targets:
  `pdftoolkit_core` (static), `pdftoolkit_ffi` (shared pure C-ABI),
  `pdftoolkit_py` (Python extension `pdftoolkit_native`, raw C-API per
  ADR-0003), `pdftoolkit_cli` (executable). Zero-warning policy enforced
  with `-Wall -Wextra -Wpedantic -Wconversion -Werror` (GCC/Clang) and
  `/W4 /permissive- /WX` (MSVC).
* **Verification evidence:** fresh configure+build → 0 warnings
  (strict `warning:|error:` grep on full build log); `ctest` 6/6 passed
  (arena, errors, ffi, c_abi_smoke, cli_version, python_import_smoke);
  `nm -D` shows the FFI library exports **exactly** the 6 `pdftoolkit_*`
  C symbols (visibility hygiene: hidden-by-default + `PDTK_API`
  annotations — an initially leaked mangled C++ symbol was found and
  eliminated); `pdftoolkit-cli --version` → `pdftoolkit-cli 0.1.0 (native
  core)`; Python import + all three extension functions work.
* **C-ABI contract established early (task 7.1 shape):** panic-proof
  (`PDTK_NOEXCEPT` + try/catch → status codes), honest stubs
  (`search_wand` → `PDTK_ERR_NOT_IMPLEMENTED`), stable doc ids for
  `register_document` (existence-validation only until task 1.1).
* **BumpArena implemented (task 1.2, partial → `in_progress`):** audit
  spec + three hardening fixes (capacity rounding for `aligned_alloc`'s
  C11 multiple-of-alignment rule; non-copyable/movable to kill the
  sketch's double-free; `count * sizeof(T)` overflow guard). 11/11 unit
  tests pass; 1M allocations + reset measured at 423,258 ns.
* **Discovery P-011:** the audit's task-1.2 acceptance bound
  (< 30 µs for 1,000,000 allocations) is physically impossible
  (30 ps/allocation); recorded with measurement and a correction request
  for the audit text. Code regression guard set at < 10 ms (intent-level).
* **Discovery U-008:** MSVC `_aligned_malloc` path is compile-only until
  a Windows CI run exists.
* **Environment note persisted:** cmake/ninja live in `.venv` (pip);
  configure needs `PATH=$PWD/.venv/bin:$PATH` — recorded in
  AGENTS.md §7 and the workflow docs.

---

## 2026-10-09 — Session 1, continued: P-007 fixed (T-004)

* **`tests/adapters/test_http.py` collection bug fixed:** replaced the
  ineffective `pytestmark = skipif(...)` + module-level fastapi import
  (which aborts the entire suite at collection when the `http` extra is
  missing) with `pytest.importorskip("fastapi")` / `("httpx")` — the same
  pattern its sibling `test_http_upload.py` already used.
* **Verification (both directions):**
  * with `http` extra: `644 passed, 1 warning` — no regression;
  * clean `.[dev]`-only venv: `589 passed, 5 skipped`, exit code 0,
    `SKIPPED` reasons all read `could not import 'fastapi'` — the
    documented "skip cleanly" behaviour now actually holds.
* **Accounting note (so the numbers never confuse anyone again):** the two
  HTTP modules hold 28 + 24 tests; a module-level skip counts as **one**
  skip entry, and 3 http-specific tests in `test_edge_cases.py` skip at
  function level → 644 − 55 = 589 passed + 5 skips.

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

---

## 2026-10-09 — Session 1, continued: issue-1/task-0.3 done (sanitizer + CI pipeline)

* **`CMakePresets.json` landed:** `release`, `debug`, `asan`
  (Address+UBSan with `-fno-sanitize-recover=all` — any finding fails the
  test), `tsan`, and `msan` (CI-only; clang + instrumented libc++
  required), each with matching build/test presets and isolated
  `build/<preset>` directories.
* **Sanitizer test exclusion policy:** the `python_import_smoke` ctest is
  excluded in sanitizer presets — loading an instrumented extension into
  an uninstrumented CPython is a toolchain-unsupported, meaningless
  configuration. Rationale documented in the preset description.
* **Verified locally (GCC 14.2):** release 6/6, debug 6/6, asan 5/5 with
  **zero sanitizer findings**, tsan 5/5 with zero findings; all builds
  had 0 compiler diagnostics. The `msan` preset fails with GCC exactly as
  P-008 documents (`cc: error: unrecognized argument to '-fsanitize='
  option: 'memory'`).
* **`.github/workflows/ci.yml` created:** push-triggered (single-branch
  policy), 4 jobs — Python suite on a 3.9/3.12 matrix, native release
  (zero-warning gate), native asan, native tsan. Includes the
  `vm.mmap_rnd_bits=28` mitigation for the known ubuntu-24.04
  sanitizer/ASLR shadow-memory runner issue. YAML-validated locally;
  **first live run pending** (tracked as U-002).
* **Discovery — timing guards are configuration-sensitive:** the arena's
  1M-allocation wall-clock regression guard tripped under ASan
  (41.6 ms vs 0.42 ms) and under Debug/-O0 (20.6 ms). Fixed by gating the
  assertion on optimized, uninstrumented builds only
  (`PDTK_TIMING_GUARDS_OFF` in `native/tests/test_arena.cpp`, covering
  `__has_feature`/`__SANITIZE_*` and `NDEBUG`). Rule added to
  dos-and-donts.md: **functional assertions always; wall-clock assertions
  only in Release without instrumentation**.

---

## 2026-10-09 — Session 1, close-out: first live CI run GREEN

* **CI confirmed on first live run** (run 37820960657, commit a9c5e98):
  all 5 job instances passed — `Python engine (pytest, py3.9)`,
  `py3.12`, `Native core (release, zero warnings)`,
  `Native core (AddressSanitizer + UBSan)`, `Native core (ThreadSanitizer)`.
  Notably this also re-verified the Python engine on **Python 3.9** (the
  oldest supported version) for the first time, and the native suite on
  GitHub's GCC — independent toolchain confirmation of the local results.
* U-002 resolved for the GCC/runner part; MSan-with-clang job remains a
  deliberate follow-up (needs instrumented libc++).
