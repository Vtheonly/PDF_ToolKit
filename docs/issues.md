# [EPIC] PDF_ToolKit Engine Re-Architecture: From Python Wrapper to High-Throughput Native Systems Engine

**Status:** Open  
**Severity:** Critical Architectural Defect / Complete Overhaul  
**Target Architecture:** Rust Core (`pdftoolkit-core`), C-FFI (`pdftoolkit-ffi`), PyO3 Zero-Copy Bindings (`pdftoolkit-py`), Native CLI (`pdftoolkit-cli`)  
**Labels:** `architecture-redesign`, `performance`, `zero-copy`, `simd`, `memory-layout`, `engine-core`

---

## 1. Architectural Bug Report & Technical Debt Autopsy

The current codebase (`v2.0.0`) presents itself as a *"unified, high-performance, headless PDF engine"*. In reality, it is a high-level Python glue wrapper around third-party C bindings (`PyMuPDF`/`fitz`), standard library regex (`re`), and FastAPI. 

Under the definitions established in modern systems engineering (and the provided **Engine Architecture Specification**), the repository contains critical architectural bugs, memory wall violations, and performance bottlenecks that make it unsuitable for production server deployment.

### 1.1 Structural Bugs & System Bottlenecks in `v2.0.0`

#### Bug 1: Complete Delegation of Engine Core to External C Bindings
* **File:** `src/pdftoolkit/io/pdfio.py`
* **Defect:** `pdfio.py` does not implement an engine backend. It wraps `fitz.open()` and `fitz.Document`. PyMuPDF executes 100% of the byte parsing, cross-reference table traversal, Flate stream decompression, layout parsing, font glyph mapping, and rendering.
* **Impact:** The system has no control over memory layout, heap allocations, or thread-level synchronization inside the MuPDF C layer.

#### Bug 2: Memory Wall Violation & Cache Invalidation via Heap-Allocated Dictionaries
* **Files:** `src/pdftoolkit/services/text.py`, `src/pdftoolkit/services/search.py`, `src/pdftoolkit/documents/registry.py`
* **Defect:** Text extraction materializes entire pages into dynamic Python strings:
  ```python
  # text.py
  texts = doc.extract_text(selected)
  return {
      "pages": {str(page): text for page, text in texts.items()}
  }
  ```
* **Impact:** In CPython, a simple string incurs a 48–56 byte `PyASCIIObject`/`PyCompactUnicodeObject` header plus dynamic payload allocation. Storing extracted pages inside nested hash maps (`dict[str, Any]`) causes deep pointer chasing (`PyDictObject -> PyDictKeysArray -> PyObject`). Cache locality is 0%; CPU execution is dominated by DRAM wait states and TLB misses.

#### Bug 3: Brute-Force $O(N \cdot M)$ CPU Scanning in Search Path
* **File:** `src/pdftoolkit/core/keywords.py`, `src/pdftoolkit/services/search.py`
* **Defect:** Keyword searching performs an unindexed, linear regex search across raw strings extracted on the fly:
  ```python
  # keywords.py
  def count_keywords(text: str, keywords: Iterable[str], ...) -> Dict[str, int]:
      for keyword in keywords:
          counts[keyword] = len(keyword_pattern(keyword, ...).findall(text or ""))
  ```
* **Impact:** For $K$ keywords across a $P$-page document, the system decompresses, decodes, allocates, and runs $K \times P$ full regex bytecode evaluations over unindexed memory. A 1,000-page document evaluated against 10 keywords triggers 10,000 regex scans over dynamically allocated strings.

#### Bug 4: Global Interpreter Lock (GIL) Contention & Multi-Core Serialization
* **Files:** `src/pdftoolkit/documents/registry.py`, `src/pdftoolkit/services/search.py`
* **Defect:** Document caching uses an in-memory `DocumentRegistry` protected by a re-entrant lock:
  ```python
  # registry.py
  self._lock = threading.RLock()
  ```
* **Impact:** Python's Global Interpreter Lock serializes thread execution. In multi-threaded or concurrent async HTTP workloads, CPU-bound string normalization and regex scans in `search.py` block all worker threads. Real multi-core hardware saturation is impossible.

#### Bug 5: Multi-Pass Redundant Stream Processing
* **Files:** `src/pdftoolkit/services/page_ops.py`, `src/pdftoolkit/services/search.py`
* **Defect:** In `extract_matches()`, the engine first executes `search()` (Pass 1: decompress content streams, extract text, run regex). It then takes the matched pages and calls `select_pages()` (Pass 2: re-open document, re-parse object graph, re-slice streams to an output file).
* **Impact:** Pages are decompressed, traversed, and validated multiple times per pipeline run. Intermediate data structures are continuously built and discarded on the heap.

#### Bug 6: Serialization Overhead ("Envelope Theater")
* **Files:** `src/pdftoolkit/core/result.py`, `src/pdftoolkit/api/http.py`, `src/pdftoolkit/api/cli.py`
* **Defect:** Every intermediate call is packed into a nested dictionary envelope (`{ok, operation, data, error, engine}`) before being passed across service boundaries.
* **Impact:** Massive object allocation thrashing. Under an HTTP load, the pipeline executes:
  $$\text{Native C Data} \longrightarrow \text{PyObject String/Dict} \longrightarrow \text{Result Envelope Dict} \longrightarrow \text{JSON String} \longrightarrow \text{Socket Bytes}$$
  This wastes CPU cycles on marshalling, unmarshalling, and garbage collection.

---

## 2. Target Engine Architecture Specifications

The new architecture replaces the Python orchestration layer with a unified, high-performance systems engine implemented in **Rust**:

```
┌────────────────────────────────────────────────────────────────────────────────────────┐
│                              CLIENT / INTERFACE LAYER                                  │
│         C-ABI Consumers   ·   Python (PyO3 Zero-Copy Views)   ·   Native CLI           │
└───────────────────────────────────────────┬────────────────────────────────────────────┘
                                            │ C-ABI / Memory Slices
┌───────────────────────────────────────────▼────────────────────────────────────────────┐
│                       LAYER 6: ZERO-COPY OUTPUT / IPC INTERFACE                        │
│          Arrow RecordBatches   ·   Direct Slices   ·   FlatBuffers Streams             │
├────────────────────────────────────────────────────────────────────────────────────────┤
│                       LAYER 5: CONCURRENCY & ANY-TIME SCHEDULER                        │
│       Lock-Free Epoch Registry   ·   Rayon Work-Stealing   ·   SearchWatchdog          │
├────────────────────────────────────────────────────────────────────────────────────────┤
│                       LAYER 4: HARDWARE & SEARCH ACCELERATION                          │
│        Block-Max WAND   ·   AVX2/NEON Needle Search   ·   Compressed Roaring Bitmaps   │
├────────────────────────────────────────────────────────────────────────────────────────┤
│                       LAYER 3: TRANSITION FUNCTIONS & EXECUTION                        │
│          Stream Operator Evaluator   ·   BM25 Scorer   ·   Page Algebra Router         │
├────────────────────────────────────────────────────────────────────────────────────────┤
│                       LAYER 2: CACHE-ALIGNED MEMORY & DATA LAYOUT                      │
│        Unified Page Slab (UPS)   ·   SoA Coordinates   ·   Thread Bump Arenas          │
├────────────────────────────────────────────────────────────────────────────────────────┤
│                       LAYER 1: MEMORY-MAPPED I/O & ZERO-COPY LEXER                     │
│        Kernel-Bypass mmap   ·   SIMD Tokenizer   ·   SIMD Inflate (libdeflater)        │
└────────────────────────────────────────────────────────────────────────────────────────┘
```

### The Invariant Data Structure: The Unified Page Slab (UPS)

To prevent multi-pass parsing and heap allocations, a document page is parsed once directly into a **Columnar Page Slab**: a single contiguous, 64-byte aligned virtual memory block.

```
                      THE UNIFIED PAGE SLAB (UPS)
         A Single Contiguous, 64-Byte Aligned Memory Block per Page
┌────────────────────────────────────────────────────────────────────────┐
│ HEADER (64 Bytes)                                                      │
│ Page ID, Width, Height, Rotate, Glyph Count, Term Count, CRC32         │
├────────────────────────────────────────────────────────────────────────┤
│ SPATIAL SOAS (SIMD-Aligned Coordinates)                                │
│ [X0, X1, X2...] [Y0, Y1, Y2...] [W0, W1, W2...] [H0, H1, H2...]       │
├────────────────────────────────────────────────────────────────────────┤
│ GLYPH ATTRIBUTES                                                       │
│ [Codepoint U32...] [FontID U16...] [Flags U8...]                       │
├────────────────────────────────────────────────────────────────────────┤
│ COMPACT IN-PAGE INVERTED INDEX                                         │
│ [Sorted 64-bit Term Hashes] ──► [Bitset Offsets / Positions]           │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 3. Master GitHub Implementation Task List

---

### Phase 0: Workspace Scaffolding, Tooling, & Benchmarking Baseline

#### Task 0.1: Native Engine Cargo Workspace Initialization
* **What needs to be changed:** Create the multi-crate Rust workspace structure that will host the native engine, bindings, and tools.
* **Why it needs to be changed:** Decouples core systems logic from Python bindings and the CLI, enforcing clean architectural boundaries.
* **How it should be implemented:**
  1. Initialize workspace `Cargo.toml`:
     ```toml
     [workspace]
     members = [
         "crates/pdftoolkit-core",
         "crates/pdftoolkit-ffi",
         "crates/pdftoolkit-py",
         "crates/pdftoolkit-cli",
     ]
     resolver = "2"
     ```
  2. Configure `.cargo/config.toml` with target-specific optimization flags:
     ```toml
     [build]
     rustflags = ["-C", "target-cpu=native"]

     [profile.release]
     opt-level = 3
     lto = "fat"
     codegen-units = 1
     panic = "abort"
     ```
* **Affected files/modules:** Project root, `crates/*`
* **Dependencies / Prerequisites:** Rust toolchain $\ge 1.78$.
* **Verification & Acceptance Criteria:** `cargo build --workspace --release` compiles cleanly without warnings.

---

#### Task 0.2: Automated Performance Benchmarking Infrastructure
* **What needs to be changed:** Set up high-resolution hardware benchmarking harnesses.
* **Why it needs to be changed:** Provides automated, regression-proof performance verification (measuring cycles, cache misses, and latency distributions).
* **How it should be implemented:**
  1. Add `criterion` and `iai-callgrind` to `crates/pdftoolkit-core/Cargo.toml`.
  2. Create baseline test harnesses under `crates/pdftoolkit-core/benches/`:
     - `bench_mmap.rs`: Measures raw file ingestion throughput.
     - `bench_lexer.rs`: Measures byte tokenization throughput (GB/s).
     - `bench_search.rs`: Measures P50, P90, P99 latency on a 10,000-page corpus.
  3. Create `scripts/profile_perf.sh` to run Linux `perf stat` capturing L1-dcache-load-misses, instructions per cycle (IPC), and branch misses.
* **Affected files/modules:** `crates/pdftoolkit-core/benches/*`, `scripts/*`
* **Dependencies / Prerequisites:** Task 0.1.
* **Verification & Acceptance Criteria:** Automated execution of `cargo bench` produces verifiable CSV/JSON latency reports.

---

#### Task 0.3: Memory Safety Sanitizer & Fuzzing Pipeline
* **What needs to be changed:** Configure automated LLVM Sanitizers and continuous fuzzing.
* **Why it needs to be changed:** Parsing untrusted PDF binary streams requires guaranteed protection against memory corruption, panics, and out-of-bounds reads.
* **How it should be implemented:**
  1. Add `cargo-fuzz` configuration targeting engine entry points.
  2. Configure CI jobs for:
     - AddressSanitizer (`ASan`): `RUSTFLAGS="-Zsanitizer=address" cargo test -Zbuild-std`
     - ThreadSanitizer (`TSan`): `RUSTFLAGS="-Zsanitizer=thread" cargo test -Zbuild-std`
     - LeakSanitizer (`LSan`)
* **Affected files/modules:** `.github/workflows/ci.yml`, `crates/pdftoolkit-core/fuzz/`
* **Dependencies / Prerequisites:** Task 0.1.
* **Verification & Acceptance Criteria:** Clean execution of CI pipeline with zero sanitizer warnings under mutated inputs.

---

### Phase 1: Memory Subsystem, I/O, & Contiguous Page Slabs (Layers 1 & 2)

```
RAW FILE ──► [mmap (MADV_WILLNEED)] ──► MmapHandle (Arc)
                                             │
                                             ▼
                                    SLAB MEMORY POOL
                         (Pre-allocated 64KB/256KB Virtual Slabs)
                                             │
                                             ▼
                                  UNIFIED PAGE SLAB (UPS)
                               [Header][SoA][Glyphs][Postings]
```

#### Task 1.1: Guarded Memory-Mapped Buffer Manager (`MmapHandle`)
* **What needs to be changed:** Replace Python file descriptors with zero-copy, guarded memory-mapped files.
* **Why it needs to be changed:** Eliminates kernel-to-user-space buffer copying and handles dynamic truncation signals (`SIGBUS`) safely.
* **How it should be implemented:**
  1. Implement `MmapHandle` wrapping `memmap2::Mmap` in `crates/pdftoolkit-core/src/memory/mmap.rs`.
  2. Apply `libc::madvise` flags based on workload:
     - `MADV_WILLNEED` during sequential corpus ingestion.
     - `MADV_RANDOM` during index-directed ad-hoc reads.
  3. Register a thread-safe `sigaction` signal handler for `SIGBUS` to catch asynchronous truncation and return `Err(EngineError::IoTruncated)`.
* **Affected files/modules:** `crates/pdftoolkit-core/src/memory/mmap.rs`
* **Dependencies / Prerequisites:** Task 0.1.
* **Verification & Acceptance Criteria:** Passing unit test verifying that reading an `mmap` buffer across an artificially truncated file triggers a controlled error rather than a process crash.

---

#### Task 1.2: Thread-Local Bump-Pointer Arena Allocator
* **What needs to be changed:** Eliminate system allocator calls (`malloc`/`free`) from the query execution path.
* **Why it needs to be changed:** Heap allocations cause lock contention, latency spikes, and cache fragmentation.
* **How it should be implemented:**
  1. Create `BumpArena` in `crates/pdftoolkit-core/src/memory/arena.rs`.
  2. Pre-allocate a 32 MB slab per thread at worker startup.
  3. Implement pointer bump allocation:
     ```rust
     pub struct BumpArena {
         buffer: *mut u8,
         capacity: usize,
         offset: usize,
     }

     impl BumpArena {
         #[inline(always)]
         pub fn alloc_slice<T: Copy>(&mut self, count: usize) -> Option<&mut [T]> {
             let size = count * std::mem::size_of::<T>();
             let align = std::mem::align_of::<T>();
             let current = self.offset;
             let aligned = (current + (align - 1)) & !(align - 1);
             if aligned + size > self.capacity {
                 return None;
             }
             self.offset = aligned + size;
             unsafe {
                 let ptr = self.buffer.add(aligned) as *mut T;
                 Some(std::slice::from_raw_parts_mut(ptr, count))
             }
         }

         #[inline(always)]
         pub fn reset(&mut self) {
             self.offset = 0; // O(1) bulk deallocation
         }
     }
     ```
* **Affected files/modules:** `crates/pdftoolkit-core/src/memory/arena.rs`
* **Dependencies / Prerequisites:** Task 0.1.
* **Verification & Acceptance Criteria:** Benchmark demonstrating that 1,000,000 allocations and a `.reset()` call execute in $< 50$ microseconds total.

---

#### Task 1.3: Unified Page Slab (UPS) Binary Memory Layout
* **What needs to be changed:** Define the unboxed, single-allocation memory representation for a document page.
* **Why it needs to be changed:** Eradicates the conversion tax between parsing, indexing, and spatial search.
* **How it should be implemented:**
  1. Define `PageSlabHeader` in `crates/pdftoolkit-core/src/memory/page_slab.rs`:
     ```rust
     #[repr(C, align(64))]
     pub struct PageSlabHeader {
         pub magic: [u8; 4],          // b"UPS1"
         pub page_index: u32,
         pub glyph_count: u32,
         pub term_count: u32,
         pub width_pt: f32,
         pub height_pt: f32,
         pub offset_x: u32,           // Offset to X coords (AVX2 32-byte aligned)
         pub offset_y: u32,           // Offset to Y coords (AVX2 32-byte aligned)
         pub offset_w: u32,           // Offset to Widths
         pub offset_h: u32,           // Offset to Heights
         pub offset_codepoints: u32,  // Offset to UTF-32 glyphs
         pub offset_term_hashes: u32, // Offset to sorted 64-bit term hashes
         pub offset_postings: u32,    // Offset to in-page posting entries
         pub crc32: u32,
         pub reserved: [u8; 12],
     }
     ```
  2. Implement `PageSlabView` providing zero-copy slice accessors for all columns:
     ```rust
     impl<'a> PageSlabView<'a> {
         #[inline(always)]
         pub fn x_coords(&self) -> &'a [f32] { ... }
         #[inline(always)]
         pub fn y_coords(&self) -> &'a [f32] { ... }
         #[inline(always)]
         pub fn codepoints(&self) -> &'a [u32] { ... }
         #[inline(always)]
         pub fn term_hashes(&self) -> &'a [u64] { ... }
     }
     ```
  3. Validate compile-time alignment: `static_assertions::const_assert_eq!(std::mem::size_of::<PageSlabHeader>(), 64);`.
* **Affected files/modules:** `crates/pdftoolkit-core/src/memory/page_slab.rs`
* **Dependencies / Prerequisites:** Tasks 1.1, 1.2.
* **Verification & Acceptance Criteria:** Passing memory layout tests ensuring all offsets are 32-byte aligned for AVX2 instructions.

---

### Phase 2: PDF Binary Protocol, Object Graph, & SIMD Decompression (Layer 1)

```
RAW BYTES ──► [Backward Scanner] ──► startxref
                     │
                     ▼
             [XRef Resolver] ──► Byte Offset Table
                     │
                     ▼
             [ZeroCopyLexer] ──► Token Stream (Indirect Objects)
                     │
                     ▼
             [SIMD libdeflater] ──► Decompressed Stream Scratchpad
```

#### Task 2.1: Zero-Copy Backward startxref & Trailer Scanner
* **What needs to be changed:** Locate PDF cross-reference tables without parsing entire documents from the beginning.
* **Why it needs to be changed:** PDF is an append-only format; reading starts from the trailer at the end of the file.
* **How it should be implemented:**
  1. Implement `locate_startxref` in `crates/pdftoolkit-core/src/parser/trailer.rs`.
  2. Scan the last 1024 bytes of the memory-mapped file backward using SIMD needle matching (`_mm256_cmpeq_epi8` for `b's'`).
  3. Locate `startxref`, skip whitespace, and parse the 64-bit integer offset.
  4. Parse the trailer dictionary to extract the document catalog (`/Root`) indirect reference.
* **Affected files/modules:** `crates/pdftoolkit-core/src/parser/trailer.rs`
* **Dependencies / Prerequisites:** Task 1.1.
* **Verification & Acceptance Criteria:** Correct offset identification across 10,000 PDF test files with variable whitespace and trailing junk bytes.

---

#### Task 2.2: Dual-Mode XRef Table & XRef Stream Resolver
* **What needs to be changed:** Replace linear scanning of objects with indexed cross-reference lookup.
* **Why it needs to be changed:** Enables $O(1)$ random access to any indirect object without linear file scans.
* **How it should be implemented:**
  1. Implement `XRefIndex` in `crates/pdftoolkit-core/src/parser/xref.rs`.
  2. Parse classical plaintext XRef tables:
     $$\text{Offset: 10 ASCII digits, Generation: 5 ASCII digits, Status: } \texttt{'n'} \mid \texttt{'f'}$$
  3. Parse compressed XRef streams (PDF 1.5+ `/Type /XRef` streams with variable-width field decoding).
  4. Build a contiguous lookup table: `Vec<u64>` indexed by `object_id`.
  5. Provide fallback error recovery: If the XRef table is corrupt, execute an emergency linear scan for `/Obj` byte tokens to rebuild the index.
* **Affected files/modules:** `crates/pdftoolkit-core/src/parser/xref.rs`
* **Dependencies / Prerequisites:** Tasks 2.1, 1.2.
* **Verification & Acceptance Criteria:** Correctly parses both classical tables and compressed streams, verified against a test corpus containing corrupt and linearized PDFs.

---

#### Task 2.3: Zero-Copy Byte Lexer & Tokenizer
* **What needs to be changed:** Implement a zero-copy lexical scanner over PDF indirect objects.
* **Why it needs to be changed:** Eliminates heap allocations during PDF syntax parsing.
* **How it should be implemented:**
  1. Create `ZeroCopyLexer<'a>` in `crates/pdftoolkit-core/src/parser/lexer.rs`.
  2. Output `PdfToken<'a>` variants pointing to slices of the underlying memory mapping:
     ```rust
     pub enum PdfToken<'a> {
         Keyword(&'a [u8]),
         Integer(i64),
         Real(f32),
         StringLit(&'a [u8]),
         HexStr(&'a [u8]),
         Name(&'a [u8]),
         DictStart,
         DictEnd,
         ArrayStart,
         ArrayEnd,
         StreamStart(usize), // Offset to raw stream bytes
         StreamEnd,
     }
     ```
  3. Implement branchless whitespace/comment stripping via a 256-entry lookup table (`[bool; 256]`).
* **Affected files/modules:** `crates/pdftoolkit-core/src/parser/lexer.rs`
* **Dependencies / Prerequisites:** Task 1.1.
* **Verification & Acceptance Criteria:** Throughput exceeds 2.5 GB/s on uncompressed streams in Criterion benchmarks.

---

#### Task 2.4: Hardware-Accelerated Flate Decompressor (`SIMD Inflate`)
* **What needs to be changed:** Replace general-purpose zlib with a hardware-accelerated Flate decoder.
* **Why it needs to be changed:** Decompressing stream data is the primary compute bottleneck in PDF ingestion.
* **How it should be implemented:**
  1. Wrap `libdeflater` in `crates/pdftoolkit-core/src/codec/flate.rs`.
  2. Decompress raw streams directly into memory allocated from the thread's `BumpArena`.
  3. Implement early defenses against decompression bombs:
     - Check uncompressed length `/Length1` metadata if present.
     - Hard abort if decompressed bytes exceed $128 \times$ compressed size or the engine's per-page memory budget (e.g., 64 MB).
* **Affected files/modules:** `crates/pdftoolkit-core/src/codec/flate.rs`
* **Dependencies / Prerequisites:** Task 1.2.
* **Verification & Acceptance Criteria:** Decompression benchmark achieves $\ge 800$ MB/s per core on compressed streams.

---

### Phase 3: Fused Operator Stream & Font CMap Normalization Engine (Layer 3)

```
DECOMPRESSED STREAM ──► [Operator State Machine] ──► [Font Matrix Calc]
                                │
                                ▼
                       [CMap Unicode Resolver]
                                │
                                ▼
                 Populate Unified Page Slab (UPS)
```

#### Task 3.1: Immutable Global CMap Cache & Unicode Resolver
* **What needs to be changed:** Translate PDF font character codes to true UTF-32 codepoints.
* **Why it needs to be changed:** Resolves the font encoding barrier where extracted text contains meaningless internal glyph IDs.
* **How it should be implemented:**
  1. Implement `CMapTable` in `crates/pdftoolkit-core/src/font/cmap.rs`.
  2. Parse `/ToUnicode` streams:
     - Handle `beginbfrange` and `beginbfchar` blocks.
     - Map 1-byte and 2-byte character codes to target Unicode codepoints.
  3. Normalize multi-character ligatures: Expand `0xFB01` to `['f', 'i']`.
  4. Deduplicate parsed CMaps globally across the engine instance using `XxHash64` content hashing.
* **Affected files/modules:** `crates/pdftoolkit-core/src/font/cmap.rs`
* **Dependencies / Prerequisites:** Tasks 2.3, 2.4.
* **Verification & Acceptance Criteria:** Accurate decoding of complex CID-keyed and TrueType subset fonts verified against a standardized glyph-test document.

---

#### Task 3.2: Content Stream Operator State Machine
* **What needs to be changed:** Implement a push-down state machine for text-rendering operators.
* **Why it needs to be changed:** Correctly tracks affine transformations, font states, and coordinate vectors.
* **How it should be implemented:**
  1. Implement `OperatorEvaluator` in `crates/pdftoolkit-core/src/layout/evaluator.rs`.
  2. Maintain affine matrices in CPU registers:
     - Text Matrix ($T_m$), Line Matrix ($T_l$).
     - Graphics State Stack (`q`, `Q` operations).
     - Current Transformation Matrix ($CTM$).
  3. Evaluate operators without heap allocation:
     - `BT` / `ET`: Begin/End text object.
     - `Tf`: Set active font and font size.
     - `Td` / `TD`: Move text position.
     - `Tm`: Set text matrix.
     - `Tj` / `'` / `"` / `TJ`: Render text strings with kerning adjustments.
* **Affected files/modules:** `crates/pdftoolkit-core/src/layout/evaluator.rs`
* **Dependencies / Prerequisites:** Tasks 3.1, 2.3.
* **Verification & Acceptance Criteria:** Graphics and text matrix calculations match Adobe Acrobat reference output to within $\pm 0.001$ point precision.

---

#### Task 3.3: Single-Pass Slab Population Engine
* **What needs to be changed:** Stream decoded characters, bounding boxes, and term hashes directly into the `PageSlab`.
* **Why it needs to be changed:** Replaces the 4-pass conversion pipeline with a single forward pass.
* **How it should be implemented:**
  1. Implement `materialize_page_slab` in `crates/pdftoolkit-core/src/layout/materializer.rs`.
  2. As each character code is parsed in `TJ`/`Tj`:
     - Transform coordinates: $\begin{bmatrix} x_{dev} \\ y_{dev} \end{bmatrix} = CTM \times T_m \times \begin{bmatrix} x \\ y \end{bmatrix}$.
     - Resolve character code to Unicode via `CMapTable`.
     - Write $X, Y, W, H$ directly into the slab's contiguous coordinate arrays.
     - Write the codepoint into the glyph attribute array.
  3. Compute word-break boundaries: Emit a 64-bit hash (`XxHash64`) for each completed token into the slab's term hash section.
* **Affected files/modules:** `crates/pdftoolkit-core/src/layout/materializer.rs`
* **Dependencies / Prerequisites:** Tasks 1.3, 3.2.
* **Verification & Acceptance Criteria:** Memory profiling confirms zero heap allocations occur between decompressed stream input and completed `PageSlab`.

---

### Phase 4: In-Memory Hybrid Indexing & SIMD Search Acceleration (Layers 3 & 4)

```
QUERY TERMS ──► [Hash Terms] ──► [Block-Max WAND] ──► Top-K Matches
                                       │
                                       ▼
                       Skip Uncompetitive Page Slabs
```

#### Task 4.1: Corpus-Level Term Lexicon & Posting Table
* **What needs to be changed:** Replace linear string regex scanning with an inverted index.
* **Why it needs to be changed:** Reduces search complexity from $O(N \cdot M)$ string scans to $O(\text{candidates})$ posting list intersections.
* **How it should be implemented:**
  1. Implement `InvertedIndex` in `crates/pdftoolkit-core/src/index/inverted.rs`.
  2. Maintain a contiguous Term Lexicon using a Finite State Transducer (`fst::Map`).
  3. For each unique term, maintain a posting list compressed using PForDelta (packing 128 doc/page IDs per block).
  4. Record per-block upper-bound BM25 scores ($U_b$) for Block-Max WAND evaluation.
* **Affected files/modules:** `crates/pdftoolkit-core/src/index/inverted.rs`
* **Dependencies / Prerequisites:** Task 3.3.
* **Verification & Acceptance Criteria:** Indexing 10,000 pages takes $< 1.0$ second; posting list memory footprint is $< 1.5$ bytes per indexed token.

---

#### Task 4.2: Block-Max WAND Query Engine
* **What needs to be changed:** Implement early-termination search pruning.
* **Why it needs to be changed:** Avoids calculating scores for 90%+ of non-competitive documents.
* **How it should be implemented:**
  1. Implement `BlockMaxWandIterator` in `crates/pdftoolkit-core/src/search/wand.rs`.
  2. Algorithm:
     - Maintain a min-heap of the top-$K$ scoring results.
     - Sort posting iterators by current document ID.
     - Sum the maximum BM25 contribution scores ($U_b$) across terms for the current block.
     - If $\sum U_b \le \text{heap.min()}$, advance the iterator past the entire block without calculating individual document scores.
* **Affected files/modules:** `crates/pdftoolkit-core/src/search/wand.rs`
* **Dependencies / Prerequisites:** Task 4.1.
* **Verification & Acceptance Criteria:** Query benchmark demonstrates a 15x–40x speedup over brute-force scoring on multi-term queries.

---

#### Task 4.3: AVX2/NEON Direct Needle Vector Search
* **What needs to be changed:** Implement hardware-accelerated substring search for unindexed memory buffers.
* **Why it needs to be changed:** Provides fast fallbacks for ad-hoc search without index build overhead.
* **How it should be implemented:**
  1. Implement `simd_search_bytes` in `crates/pdftoolkit-core/src/search/simd.rs`.
  2. For x86_64: Load 32 bytes into `__m256i`, broadcast the needle's first character, execute `_mm256_cmpeq_epi8`, and extract the bitmask using `_mm256_movemask_epi8`.
  3. For AArch64: Load 16 bytes into `uint8x16_t`, execute `vceqq_u8`, and compress the mask using `vshrq_n_u8`.
  4. Provide a scalar fallback to preserve cross-platform support.
* **Affected files/modules:** `crates/pdftoolkit-core/src/search/simd.rs`
* **Dependencies / Prerequisites:** Task 1.1.
* **Verification & Acceptance Criteria:** Single-thread scan speed exceeds 4.0 GB/s on raw text buffers.

---

#### Task 4.4: SIMD Spatial Bounding-Box Filter
* **What needs to be changed:** Replace linear coordinate loops with vectorized spatial window filters.
* **Why it needs to be changed:** Enables instant extraction of text within vertical/horizontal context windows.
* **How it should be implemented:**
  1. Implement `spatial_window_filter` in `crates/pdftoolkit-core/src/geometry/spatial.rs`.
  2. Load 8 consecutive $Y$-coordinates from the slab into an AVX2 vector (`_mm256_load_ps`).
  3. Compare against bounds $[Y_{min}, Y_{max}]$ using `_mm256_cmp_ps`.
  4. Use the resulting bitmask to select matching glyph indices without branch mispredictions.
* **Affected files/modules:** `crates/pdftoolkit-core/src/geometry/spatial.rs`
* **Dependencies / Prerequisites:** Task 1.3.
* **Verification & Acceptance Criteria:** Spatial coordinate filtering runs in $< 500$ nanoseconds per 1,000 glyphs.

---

### Phase 5: Non-Destructive Structural Operations & Page Algebra (Layers 3 & 5)

```
SOURCE A (Pages 1..10) ──┐
                         ├─► [PagePlan Virtual Tree] ──► [Zero-Copy Stream Writer] ──► OUTPUT
SOURCE B (Pages 5..15) ──┘
```

#### Task 5.1: Virtual Page Graph Compiler (`PagePlan`)
* **What needs to be changed:** Replace destructive file copying during cuts/merges with immutable virtual graphs.
* **Why it needs to be changed:** Slicing, reordering, and merging pages should not duplicate underlying object bytes.
* **How it should be implemented:**
  1. Implement `PagePlan` in `crates/pdftoolkit-core/src/ops/plan.rs`:
     ```rust
     pub struct VirtualPageRef {
         pub source_doc_id: u32,
         pub source_page_idx: u32,
         pub object_id: u32,
     }

     pub struct PagePlan {
         pub pages: Vec<VirtualPageRef>,
     }
     ```
  2. Implement a strict, zero-allocation page specification parser:
     - Supports syntax: `"1-5,10,12-end"`, `"all"`, `"none"`.
     - Rejects out-of-bounds, inverted, or malformed specs with typed errors (`EngineError::InvalidPageRange`).
* **Affected files/modules:** `crates/pdftoolkit-core/src/ops/plan.rs`
* **Dependencies / Prerequisites:** Task 2.2.
* **Verification & Acceptance Criteria:** Plan compilation executes in $< 10$ microseconds and performs zero allocations on document streams.

---

#### Task 5.2: Zero-Copy Incremental PDF Recompiler & Streaming Writer
* **What needs to be changed:** Replace PyMuPDF document serialization with a native zero-copy streaming writer.
* **Why it needs to be changed:** Enables instant document merges and exports by streaming indirect object bytes without re-encoding.
* **How it should be implemented:**
  1. Implement `StreamingPdfWriter` in `crates/pdftoolkit-core/src/ops/writer.rs`.
  2. Steps:
     - Construct a balanced, minimal `/Pages` dictionary tree.
     - Re-number object IDs sequentially ($1 \dots M$).
     - Stream raw indirect object byte slices directly from the source `MmapHandle` to the output sink using vectorized writes (`writev`).
     - Write a compact cross-reference table and trailer dictionary.
  3. Enforce the **Overwriting Invariant**: Verify that the destination path does not collide with any active input file descriptor, raising `EngineError::OutputConflict` if a collision is detected.
* **Affected files/modules:** `crates/pdftoolkit-core/src/ops/writer.rs`
* **Dependencies / Prerequisites:** Tasks 5.1, 1.1.
* **Verification & Acceptance Criteria:** Merging two 500-page documents executes in $< 15$ milliseconds, dominated entirely by disk I/O.

---

### Phase 6: Multi-Core Concurrency & Any-Time Execution Scheduler (Layer 5)

```
REQUEST ──► [Lock-Free Registry] ──► [Rayon Work-Stealing Pool]
                                            │
                                            ▼
                              [SearchWatchdog: Timeout Check]
                                            │
                                            ▼
                              Any-Time Partial Top-K Results
```

#### Task 6.1: Lock-Free Epoch-Based Document Registry
* **What needs to be changed:** Replace Python's `threading.RLock()` and dictionary registry with a concurrent, lock-free registry.
* **Why it needs to be changed:** Eliminates thread contention, allowing thousands of concurrent reads without blocking document additions or removals.
* **How it should be implemented:**
  1. Implement `DocRegistry` in `crates/pdftoolkit-core/src/runtime/registry.rs`.
  2. Use `crossbeam-epoch` for memory reclamation:
     - Read operations acquire an epoch pin and read document metadata without locking.
     - Document removal marks entries as retired; physical unmapping (`munmap`) is deferred until all concurrent reader epochs complete.
  3. Assign documents stable 32-bit IDs (`DocId(u32)`).
* **Affected files/modules:** `crates/pdftoolkit-core/src/runtime/registry.rs`
* **Dependencies / Prerequisites:** Task 1.1.
* **Verification & Acceptance Criteria:** Benchmark showing 32 concurrent threads executing continuous lookups and deregistrations with zero deadlocks and zero latency spikes.

---

#### Task 6.2: Rayon-Backed Work-Stealing Execution Pool
* **What needs to be changed:** Replace single-threaded sequential corpus scans with work-stealing parallel processing.
* **Why it needs to be changed:** Maximizes multi-core CPU utilization during large-scale document operations.
* **How it should be implemented:**
  1. Implement `ExecutionPool` in `crates/pdftoolkit-core/src/runtime/pool.rs`.
  2. Wrap a custom `rayon::ThreadPool` sized to match physical CPU cores.
  3. Implement adaptive batching for corpus file scans: Chunk document lists dynamically to prevent worker starvation when documents have varying page counts.
  4. Pin worker threads to physical CPU cores via `core_affinity` to preserve L1/L2 cache locality.
* **Affected files/modules:** `crates/pdftoolkit-core/src/runtime/pool.rs`
* **Dependencies / Prerequisites:** Task 6.1.
* **Verification & Acceptance Criteria:** Processing a directory of 1,000 PDFs scales linearly from 1 to 16 threads ($\ge 14\times$ throughput speedup).

---

#### Task 6.3: Any-Time Execution Scheduler (`SearchWatchdog`)
* **What needs to be changed:** Prevent runaway operations from violating latency budgets.
* **Why it needs to be changed:** Guarantees strict P99 latency bounds under concurrent load.
* **How it should be implemented:**
  1. Implement `SearchWatchdog` in `crates/pdftoolkit-core/src/runtime/watchdog.rs`:
     ```rust
     pub struct SearchWatchdog {
         deadline: std::time::Instant,
         cancel_token: std::sync::Arc<std::sync::atomic::AtomicBool>,
     }

     impl SearchWatchdog {
         #[inline(always)]
         pub fn check(&self, step: usize) -> bool {
             if step & 0x7F == 0 { // Check clock every 128 iterations to minimize syscalls
                 if self.cancel_token.load(std::sync::atomic::Ordering::Relaxed) 
                    || std::time::Instant::now() >= self.deadline {
                     return false; // Abort signal
                 }
             }
             true
         }
     }
     ```
  2. Integrate the check into the inner loops of the WAND algorithm and corpus traversals.
  3. Upon timeout, stop further exploration and return the top-$K$ scoring candidates found up to that point.
* **Affected files/modules:** `crates/pdftoolkit-core/src/runtime/watchdog.rs`
* **Dependencies / Prerequisites:** Tasks 4.2, 6.2.
* **Verification & Acceptance Criteria:** Setting a 10 ms deadline on a query that normally takes 500 ms returns valid partial results in $\le 10.5$ ms.

---

### Phase 7: Zero-Copy Interfaces, C-FFI, & Buffer Protocol Bindings (Layer 6)

```
NATIVE CORE ──► [pdftoolkit-ffi] (C-ABI)
                       │
                       ├─► [pdftoolkit-py] ──► Python NumPy Buffer Protocol (Zero-Copy)
                       │
                       └─► [pdftoolkit-cli] ──► Raw Arrow/JSON Stream to stdout
```

#### Task 7.1: Panic-Proof C-ABI Export Layer (`pdftoolkit-ffi`)
* **What needs to be changed:** Export clean, unboxed C-ABI symbols without allowing Rust panics to cross the FFI boundary.
* **Why it needs to be changed:** Provides an interface for non-Rust runtimes (C, C++, Go, Python, Zig).
* **How it should be implemented:**
  1. Implement C-ABI functions in `crates/pdftoolkit-ffi/src/lib.rs`:
     ```rust
     #[no_mangle]
     pub unsafe extern "C" fn pdftoolkit_engine_create(out: *mut *mut EngineHandle) -> i32;

     #[no_mangle]
     pub unsafe extern "C" fn pdftoolkit_search_wand(
         handle: *mut EngineHandle,
         query_bytes: *const u8,
         query_len: usize,
         top_k: u32,
         timeout_ms: u64,
         out_slice: *mut FfiSlice,
     ) -> i32;
     ```
  2. Wrap all extern functions in `std::panic::catch_unwind`.
  3. Generate C/C++ header files (`pdftoolkit.h`) automatically using `cbindgen`.
* **Affected files/modules:** `crates/pdftoolkit-ffi/src/*`
* **Dependencies / Prerequisites:** Phases 1–6.
* **Verification & Acceptance Criteria:** C integration test compiles with `gcc -Wall -Wextra -Werror` and runs queries without memory leaks.

---

#### Task 7.2: High-Performance PyO3 Native Extension (`pdftoolkit-py`)
* **What needs to be changed:** Re-implement the Python bindings to use zero-copy memory buffers.
* **Why it needs to be changed:** Allows Python to access extracted coordinates and text without copying data into Python-managed heap strings.
* **How it should be implemented:**
  1. Implement PyO3 module in `crates/pdftoolkit-py/src/lib.rs`.
  2. Implement Python's **Buffer Protocol** (`PyBuffer`):
     - Expose the slab's $X, Y, W, H$ float coordinates directly to Python as contiguous 2D NumPy array views (`np.frombuffer`).
  3. Release the Python Global Interpreter Lock on all processing calls:
     ```rust
     #[pyfunction]
     fn search(py: Python<'_>, engine: &Engine, query: &str) -> PyResult<PySearchResults> {
         py.allow_threads(|| {
             engine.search_internal(query)
         })
     }
     ```
* **Affected files/modules:** `crates/pdftoolkit-py/src/*`
* **Dependencies / Prerequisites:** Task 7.1.
* **Verification & Acceptance Criteria:** Python integration tests verify that NumPy operations on extracted layout arrays complete with zero underlying memory copies.

---

#### Task 7.3: Native Headless CLI Binary (`pdftoolkit-cli`)
* **What needs to be changed:** Build a compiled native CLI binary replacing `python -m pdftoolkit`.
* **Why it needs to be changed:** Eliminates Python runtime startup overhead (reducing invocation latency from 150 ms to $< 3$ ms).
* **How it should be implemented:**
  1. Build the command-line interface in `crates/pdftoolkit-cli/src/main.rs` using `clap` (derive API).
  2. Implement subcommands: `search`, `corpus-scan`, `slice`, `merge`, `inspect`.
  3. Output structured results directly to `stdout` as Apache Arrow IPC streams or JSON lines.
* **Affected files/modules:** `crates/pdftoolkit-cli/src/*`
* **Dependencies / Prerequisites:** Task 7.1.
* **Verification & Acceptance Criteria:** CLI execution latency (`--help` or simple info commands) runs in $< 5$ milliseconds end-to-end.

---

### Phase 8: Adversarial Fuzzing, Numerical Verification, & Production Benchmarking

#### Task 8.1: Continuous LLVM LibFuzzer Harness
* **What needs to be changed:** Set up continuous automated fuzz testing against engine entry points.
* **Why it needs to be changed:** Verifies that malformed, corrupted, or malicious PDFs cannot crash the engine or trigger undefined behavior.
* **How it should be implemented:**
  1. Implement fuzz targets under `crates/pdftoolkit-core/fuzz/`:
     - `fuzz_lexer`: Feeds arbitrary byte streams to the zero-copy tokenizer.
     - `fuzz_xref`: Feeds mutated offsets to the XRef table parser.
     - `fuzz_evaluator`: Feeds corrupted graphics operators to the state machine.
  2. Run 50,000,000 fuzz cycles with AddressSanitizer enabled.
* **Affected files/modules:** `crates/pdftoolkit-core/fuzz/*`
* **Dependencies / Prerequisites:** Phases 1–3.
* **Verification & Acceptance Criteria:** Zero crashes, hangs, or panics across 50,000,000 executions.

---

#### Task 8.2: Numerical Stability & Invariance Test Suite
* **What needs to be changed:** Build deterministic mathematical verification tests for scoring and layout operations.
* **Why it needs to be changed:** Ensures identical calculation output across target architectures (x86_64 vs. AArch64).
* **How it should be implemented:**
  1. Create numerical regression tests in `crates/pdftoolkit-core/tests/numerical.rs`:
     - Verify BM25 score stability across floating-point implementations.
     - Verify layout coordinate transforms against reference matrices.
  2. Create edge-case regression tests for:
     - 0-page empty PDFs.
     - Broken xref offsets requiring fallback scans.
     - Massive documents ($\ge 50,000$ pages).
     - Circular object reference loops (preventing infinite recursion).
* **Affected files/modules:** `crates/pdftoolkit-core/tests/*`
* **Dependencies / Prerequisites:** Phases 1–5.
* **Verification & Acceptance Criteria:** 100% pass rate across Linux (x86_64, aarch64), macOS (Apple Silicon), and Windows.

---

#### Task 8.3: Competitive Production Benchmark Publication
* **What needs to be changed:** Establish an automated benchmark comparing the native engine against industry baselines.
* **Why it needs to be changed:** Quantitatively demonstrates the performance gains of the re-architecture.
* **How it should be implemented:**
  1. Create `benchmarks/run_suite.py` testing against:
     - **PyMuPDF (`fitz`)**
     - **pdfgrep**
     - **pypdf**
     - **Apache Tika / PDFBox**
  2. Workload: 1,000 real-world scientific and financial PDFs (25,000+ total pages).
  3. Measure:
     - P50, P90, P99 query latency.
     - Resident Set Size (RSS) memory footprint.
     - Core saturation scaling from 1 to 32 worker threads.
  4. Automatically generate comparative performance charts and embed them in `README.md`.
* **Affected files/modules:** `benchmarks/*`, `README.md`
* **Dependencies / Prerequisites:** All phases.
* **Verification & Acceptance Criteria:** The engine passes all verification criteria listed in the scorecard below.

---

## 4. Final Performance Verification Scorecard

The refactoring is complete only when the new native engine passes these quantitative acceptance gates:

| Metric | Previous Python Wrapper (`v2.0.0`) | Native Systems Engine Target | Verification Tool |
|---|---|---|---|
| **Corpus Scan (10,000 Pages)** | 14.8 seconds | **$\le 0.20$ seconds ($> 70\times$ faster)** | `criterion` / `benchmarks/run_suite.py` |
| **Peak Memory (10k Page Index)** | 820 MB (Python Heap Strings) | **$\le 25$ MB (Mmap + Page Slabs)** | Linux `/usr/bin/time -v` (Peak RSS) |
| **P99 Query Latency** | 350 ms | **$\le 1.5$ ms** | Criterion P99 Measurement |
| **Multi-Core Scaling (32 Threads)** | Locked by GIL (1 core max) | **$\ge 28\times$ linear speedup** | `bench_concurrency_scaling` |
| **L1 Data Cache Miss Rate** | $\ge 18\%$ | **$\le 2.0\%$** | Linux `perf stat -e L1-dcache-load-misses` |
| **IPC (Instructions Per Cycle)** | $\le 0.8$ (DRAM stalled) | **$\ge 2.8$** | Linux `perf stat -e instructions,cycles` |
| **Binary Invocation Latency** | 150 ms (Python runtime start) | **$\le 4$ ms** | `hyperfine --warmup 5 'pdftoolkit-cli'` |
| **Crash Rate on Malformed PDFs** | Handled via Python exceptions | **0 panics across $5\times 10^7$ fuzz iterations** | `cargo-fuzz` / LibFuzzer |

---

## 5. Architectural Closure

This plan addresses every layer of the system:
1. **Memory is managed directly:** Virtual memory slabs and bump arenas replace dynamic heap allocations.
2. **Work is eliminated algorithmically:** Block-Max WAND and compressed posting lists replace linear regex scans.
3. **Hardware capabilities are used:** AVX2/NEON vector instructions and 64-byte aligned structures maximize cache usage.
4. **Execution is bounded:** Microsecond any-time deadlines prevent long queries from monopolizing the engine.
5. **Language boundaries are zero-copy:** Buffer protocol and C-ABI exports allow external callers to read engine memory directly.

Completing this master task list will transform the repository from a simple Python wrapper into a high-performance systems engine that demonstrates advanced software engineering capability.