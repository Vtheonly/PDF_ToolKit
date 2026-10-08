# [EPIC] PDF_ToolKit Architectural Overhaul: Native C++20 Systems Engine with Zero-Copy Python Bindings

**Status:** Open  
**Severity:** Critical Architectural Overhaul  
**Target Architecture:** C++20 Native Core (`pdftoolkit-core`), C-ABI (`pdftoolkit-ffi`), Python Buffer Protocol Bindings (`pdftoolkit-py`), Native CLI (`pdftoolkit-cli`)  
**Labels:** `architecture-redesign`, `cpp20`, `python-c-api`, `simd`, `zero-copy`, `memory-wall`

---

## 1. Architectural Bug Report & Technical Debt Autopsy

The current repository (`v2.0.0`) is a high-level Python glue wrapper around third-party C bindings (`PyMuPDF`/`fitz`), standard library regex (`re`), and FastAPI. 

Per the **Engine Architecture Specification** (specifically **Chapter 1.1: Defining the Engine Concept**, **Chapter 4.1: Primacy of Memory**, and **Chapter 7.1: Low-Level System Languages**), the current codebase suffers from five fatal architectural flaws that disqualify it from being an engine:

```
CURRENT ARCHITECTURE (UNSCALABLE GLUE):
[Disk PDF] ──► [fitz.open (C Heap)] ──► [extract_text()] ──► [Heap Python Strings]
                                                                     │
                         ┌───────────────────────────────────────────┘
                         ▼
           [re.findall() Linear Scan] ──► [Dict Envelopes] ──► [JSON Serializer]
           ▲                              ▲
           └────── $O(N \cdot M)$ SCAN ───┴── 0% CACHE LOCALITY (POINTER CHASING)
```

### 1.1 The Specific Defects in `v2.0.0`

1. **The Delegation Defect (`io/pdfio.py`):**  
   The codebase implements zero PDF parsing, zero stream decompression, and zero font processing. It delegates 100% of the byte traversal to MuPDF’s shared library. It cannot control memory layout, allocation arenas, or thread-level scheduling.
2. **The Memory Wall Defect (`services/text.py`, `services/search.py`):**  
   Pages are materialized into CPython `PyUnicode` objects on the heap. In CPython, each string has a 48–56 byte header plus dynamic payload allocation, scattered across virtual memory. Storing extracted pages in Python dictionaries (`dict[str, Any]`) causes deep pointer chasing (`PyDictObject -> PyDictKeysArray -> PyObject`). Cache locality is 0%; the CPU sits idle in memory wait states.
3. **The Algorithmic Brute-Force Defect (`core/keywords.py`):**  
   Search is implemented as an unindexed regex scan:
   ```python
   counts[keyword] = len(keyword_pattern(keyword).findall(text or ""))
   ```
   For $K$ keywords over $P$ pages, it decompresses and regex-scans memory $K \times P$ times. A 1,000-page document evaluated against 10 keywords executes 10,000 unindexed regex evaluations over heap strings.
4. **The Global Interpreter Lock (GIL) Defect (`documents/registry.py`):**  
   The `DocumentRegistry` synchronizes state using Python's `threading.RLock()`. Under concurrent server queries, CPython's GIL serializes all CPU execution onto a single core. Multi-core scaling is physically impossible.
5. **The Marshalling Defect (`core/result.py`, `api/http.py`):**  
   Every operation serializes results into heap-allocated dictionary envelopes (`{ok, operation, data, error, engine}`) before passing them to FastAPI’s `json.dumps()`. This creates massive memory thrashing:
   $$\text{Native C Data} \longrightarrow \text{PyObject String/Dict} \longrightarrow \text{Envelope Dict} \longrightarrow \text{JSON String} \longrightarrow \text{Socket}$$

---

## 2. Target Engine Architecture (C++20 Core + Python Buffer Protocol)

The new architecture replaces the Python orchestration layer with a unified, high-performance systems engine implemented in **C++20**, exposing its memory directly to Python via the **Python Buffer Protocol (`Py_buffer`)** and **C-ABI**:

```
┌────────────────────────────────────────────────────────────────────────────────────────┐
│                                APPLICATION / CLIENT TIER                               │
│        Python (NumPy / Zero-Copy Buffer Protocol)   ·   C-ABI Clients   ·   CLI        │
└───────────────────────────────────────────┬────────────────────────────────────────────┘
                                            │ std::span / Raw Pointer Slices
┌───────────────────────────────────────────▼────────────────────────────────────────────┐
│                       LAYER 6: ZERO-COPY OUTPUT / IPC INTERFACE                        │
│      Python Buffer Protocol (Py_buffer)   ·   writev Vectors   ·   Arrow IPC Export    │
├────────────────────────────────────────────────────────────────────────────────────────┤
│                       LAYER 5: CONCURRENCY & ANY-TIME SCHEDULER                        │
│   Lock-Free Epoch Registry (Atomic)  ·  Work-Stealing Pool  ·  SearchWatchdog (Timer)  │
├────────────────────────────────────────────────────────────────────────────────────────┤
│                       LAYER 4: HARDWARE & SEARCH ACCELERATION                          │
│     Block-Max WAND Pruning   ·   AVX2/NEON Needle Search   ·   PForDelta Postings      │
├────────────────────────────────────────────────────────────────────────────────────────┤
│                       LAYER 3: TRANSITION FUNCTIONS & EXECUTION                        │
│      Stream Operator Evaluator   ·   BM25 Scorer   ·   Virtual PagePlan Compiler       │
├────────────────────────────────────────────────────────────────────────────────────────┤
│                       LAYER 2: CACHE-ALIGNED MEMORY & DATA LAYOUT                      │
│     Unified Page Slab (alignas(64))   ·   SoA Coordinates   ·   Thread Bump Arenas     │
├────────────────────────────────────────────────────────────────────────────────────────┤
│                       LAYER 1: MEMORY-MAPPED I/O & ZERO-COPY LEXER                     │
│     POSIX/Win32 mmap   ·   SIMD Tokenizer   ·   Hardware Inflate (libdeflater)         │
└────────────────────────────────────────────────────────────────────────────────────────┘
```

### The Invariant Data Structure: The Unified Page Slab (UPS)

All operations (parsing, spatial queries, indexing, and Python export) operate on a single, contiguous, 64-byte aligned virtual memory block: the **Unified Page Slab (UPS)**. The parser writes directly into this slab; spatial queries, search routines, and Python zero-copy views read directly from it. **Zero conversions. Zero intermediate objects.**

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

### Phase 0: Workspace Scaffolding, Tooling, & Modern C++20 Infrastructure

#### Task 0.1: Build System Setup (CMake 3.25+ & Ninja)
* **What needs to be changed:** Create a modular CMake build infrastructure targeting C++20 across Linux, macOS, and Windows.
* **Why it needs to be changed:** Modern C++ requires explicit build isolation, strict compiler warnings, and target optimization flags (`-O3`, `-march=native`).
* **How it should be implemented:**
  1. Create root `CMakeLists.txt` structuring the project into modular targets:
     - `pdftoolkit_core`: Static systems library (`cxx_std_20`).
     - `pdftoolkit_ffi`: Shared library exporting pure C-ABI symbols.
     - `pdftoolkit_py`: Python native extension module (`pybind11` / Python C-API).
     - `pdftoolkit_cli`: Native compiled executable.
  2. Configure aggressive release optimization flags:
     ```cmake
     set(CMAKE_CXX_STANDARD 20)
     set(CMAKE_CXX_STANDARD_REQUIRED ON)
     if (MSVC)
         add_compile_options(/O2 /arch:AVX2 /W4 /permissive-)
     else()
         add_compile_options(-O3 -march=native -Wall -Wextra -Wpedantic -Wconversion -fno-omit-frame-pointer)
     endif()
     ```
* **Affected files/modules:** `CMakeLists.txt`, `cmake/*`
* **Dependencies / Prerequisites:** Clang 16+, GCC 13+, or MSVC 2022.
* **Verification & Acceptance Criteria:** `cmake -B build -G Ninja && ninja -C build` builds all four targets with zero warnings.

---

#### Task 0.2: Automated Benchmarking Pipeline (Google Benchmark)
* **What needs to be changed:** Build nanosecond-precision micro- and macro-benchmarking suites.
* **Why it needs to be changed:** Verifies memory throughput, instruction count, and latency distributions (P50, P90, P99).
* **How it should be implemented:**
  1. Integrate `google/benchmark` via `FetchContent`.
  2. Implement benchmark targets in `benchmarks/`:
     - `bench_mmap.cpp`: Measures file read throughput (GB/s).
     - `bench_lexer.cpp`: Measures tokenization rate (GB/s).
     - `bench_slab.cpp`: Measures cache hit rate and coordinate extraction latency.
     - `bench_wand.cpp`: Measures query execution time over 10,000 pages.
  3. Script Linux `perf stat` automation to record `L1-dcache-load-misses`, `instructions`, and `cycles`.
* **Affected files/modules:** `benchmarks/*`, `scripts/run_perf.sh`
* **Dependencies / Prerequisites:** Task 0.1.
* **Verification & Acceptance Criteria:** Automated execution of `./build/benchmarks/bench_wand` outputs statistical latency curves.

---

#### Task 0.3: Sanitizers and Hardening Pipeline
* **What needs to be changed:** Configure automated LLVM Sanitizers for continuous CI runs.
* **Why it needs to be changed:** Parsing raw byte streams in C++ requires continuous automated verification against out-of-bounds reads, memory leaks, and undefined behavior.
* **How it should be implemented:**
  1. Add sanitizer presets in `CMakePresets.json`:
     - AddressSanitizer (`-fsanitize=address,undefined`)
     - ThreadSanitizer (`-fsanitize=thread`)
     - MemorySanitizer (`-fsanitize=memory`)
  2. Configure `.github/workflows/ci.yml` to run test suites under all sanitizers.
* **Affected files/modules:** `CMakePresets.json`, `.github/workflows/ci.yml`
* **Dependencies / Prerequisites:** Task 0.1.
* **Verification & Acceptance Criteria:** Clean execution of the full test suite with zero sanitizer errors.

---

### Phase 1: Native Memory Subsystem & Virtual Page Slabs (Layers 1 & 2)

```
RAW FILE ──► [POSIX mmap / Win32 MapViewOfFile] ──► MmapHandle (std::shared_ptr)
                                                           │
                                                           ▼
                                                  SLAB MEMORY POOL
                                       (Pre-allocated 64KB/256KB Slabs)
                                                           │
                                                           ▼
                                                UNIFIED PAGE SLAB (UPS)
                                             [Header][SoA][Glyphs][Postings]
```

#### Task 1.1: Guarded Memory-Mapped Buffer Manager (`MmapHandle`)
* **What needs to be changed:** Replace Python file handlers with a zero-copy, guarded memory-mapped file abstraction.
* **Why it needs to be changed:** Eliminates kernel-to-user-space buffer copying and handles dynamic file truncation safely.
* **How it should be implemented:**
  1. Create `MmapHandle` in `include/pdftoolkit/memory/mmap.hpp` and `src/memory/mmap.cpp`.
  2. Use POSIX `mmap` / Win32 `CreateFileMappingA` + `MapViewOfFile`.
  3. Apply OS read-ahead hints via `madvise` / `posix_madvise`:
     - `MADV_WILLNEED` during sequential corpus ingestion.
     - `MADV_RANDOM` during index-directed ad-hoc reads.
  4. Register a thread-safe `sigaction` handler for `SIGBUS` to catch asynchronous file truncation and throw `PdfToolkitException(ErrorCode::IoTruncated)`.
  5. Expose memory exclusively as immutable `std::span<const uint8_t>`.
* **Affected files/modules:** `include/pdftoolkit/memory/mmap.hpp`, `src/memory/mmap.cpp`
* **Dependencies / Prerequisites:** Task 0.1.
* **Verification & Acceptance Criteria:** Unit test verifying that accessing an `MmapHandle` across an artificially truncated file throws a controlled exception rather than a process crash.

---

#### Task 1.2: Thread-Local Monolithic Bump-Pointer Arena Allocator
* **What needs to be changed:** Eliminate system allocator calls (`malloc`/`free`/`new`) from the query execution path.
* **Why it needs to be changed:** Heap allocations cause lock contention, latency spikes, and cache fragmentation.
* **How it should be implemented:**
  1. Create `BumpArena` in `include/pdftoolkit/memory/arena.hpp`:
     ```cpp
     class BumpArena {
     private:
         uint8_t* buffer_;
         size_t capacity_;
         size_t offset_;
     public:
         explicit BumpArena(size_t capacity)
             : capacity_(capacity), offset_(0) {
             buffer_ = static_cast<uint8_t*>(std::aligned_alloc(64, capacity));
         }
         ~BumpArena() { std::free(buffer_); }

         template <typename T>
         std::span<T> alloc_slice(size_t count) noexcept {
             size_t bytes = count * sizeof(T);
             size_t align = alignof(T);
             size_t current = offset_;
             size_t aligned = (current + (align - 1)) & ~(align - 1);
             if (aligned + bytes > capacity_) return {};
             offset_ = aligned + bytes;
             return std::span<T>(reinterpret_cast<T*>(buffer_ + aligned), count);
         }

         void reset() noexcept { offset_ = 0; } // O(1) bulk deallocation
     };
     ```
  2. Pre-allocate a 32 MB slab per thread at worker startup.
* **Affected files/modules:** `include/pdftoolkit/memory/arena.hpp`, `src/memory/arena.cpp`
* **Dependencies / Prerequisites:** Task 0.1.
* **Verification & Acceptance Criteria:** Benchmark demonstrating that 1,000,000 slice allocations and a `.reset()` call execute in $< 30$ microseconds total.

---

#### Task 1.3: Unified Page Slab (UPS) Binary Memory Layout
* **What needs to be changed:** Define the unboxed, single-allocation memory representation for a document page.
* **Why it needs to be changed:** Eliminates data conversion steps between parsing, indexing, and spatial search.
* **How it should be implemented:**
  1. Define `PageSlabHeader` in `include/pdftoolkit/memory/page_slab.hpp`:
     ```cpp
     struct alignas(64) PageSlabHeader {
         uint8_t  magic[4];           // "UPS1"
         uint32_t page_index;
         uint32_t glyph_count;
         uint32_t term_count;
         float    width_pt;
         float    height_pt;
         uint32_t offset_x;           // AVX2 32-byte aligned offset
         uint32_t offset_y;           // AVX2 32-byte aligned offset
         uint32_t offset_w;           // AVX2 32-byte aligned offset
         uint32_t offset_h;           // AVX2 32-byte aligned offset
         uint32_t offset_codepoints;  // Offset to UTF-32 glyphs
         uint32_t offset_term_hashes; // Offset to sorted 64-bit term hashes
         uint32_t offset_postings;    // Offset to in-page posting entries
         uint32_t crc32;
         uint8_t  reserved[12];
     };
     static_assert(sizeof(PageSlabHeader) == 64, "Header must occupy exactly one cache line");
     ```
  2. Implement `PageSlabView` providing zero-copy `std::span` accessors for all coordinate and attribute arrays.
* **Affected files/modules:** `include/pdftoolkit/memory/page_slab.hpp`
* **Dependencies / Prerequisites:** Tasks 1.1, 1.2.
* **Verification & Acceptance Criteria:** Unit tests verifying that coordinate offsets are 32-byte aligned for AVX2 instructions.

---

### Phase 2: PDF Binary Protocol, Object Graph, & SIMD Decompression (Layer 1)

```
RAW BYTES ──► [Backward Scanner] ──► startxref
                     │
                     ▼
             [XRef Resolver] ──► Byte Offset Table
                     │
                     ▼
             [ZeroCopyLexer] ──► std::string_view Tokens
                     │
                     ▼
             [SIMD libdeflater] ──► Decompressed Stream Scratchpad
```

#### Task 2.1: Zero-Copy Backward startxref & Trailer Scanner
* **What needs to be changed:** Locate PDF cross-reference tables without parsing entire documents from the beginning.
* **Why it needs to be changed:** PDF is an append-only format; reading starts from the trailer at the end of the file.
* **How it should be implemented:**
  1. Implement `locate_startxref` in `src/parser/trailer.cpp`.
  2. Scan the last 1024 bytes of the memory-mapped file backward using SIMD needle matching (`_mm256_cmpeq_epi8` for `b's'`).
  3. Parse the 64-bit integer offset of `startxref`.
  4. Parse the trailer dictionary to extract the document catalog (`/Root`) reference.
* **Affected files/modules:** `include/pdftoolkit/parser/trailer.hpp`, `src/parser/trailer.cpp`
* **Dependencies / Prerequisites:** Task 1.1.
* **Verification & Acceptance Criteria:** Correct offset identification across 10,000 PDF test files with variable whitespace and trailing junk bytes.

---

#### Task 2.2: Dual-Mode XRef Table & XRef Stream Resolver
* **What needs to be changed:** Replace linear scanning of objects with indexed cross-reference lookup.
* **Why it needs to be changed:** Enables $O(1)$ random access to any indirect object without linear file scans.
* **How it should be implemented:**
  1. Implement `XRefIndex` in `src/parser/xref.cpp`.
  2. Parse classical plaintext XRef tables:
     $$\text{Offset: 10 ASCII digits, Generation: 5 ASCII digits, Status: } \texttt{'n'} \mid \texttt{'f'}$$
  3. Parse compressed XRef streams (PDF 1.5+ `/Type /XRef` streams with variable-width field decoding).
  4. Build a contiguous lookup table: `std::vector<uint64_t>` indexed by `object_id`.
  5. Provide fallback error recovery: If the XRef table is corrupt, execute an emergency linear scan for `/Obj` byte tokens to rebuild the index.
* **Affected files/modules:** `include/pdftoolkit/parser/xref.hpp`, `src/parser/xref.cpp`
* **Dependencies / Prerequisites:** Tasks 2.1, 1.2.
* **Verification & Acceptance Criteria:** Correctly parses both classical tables and compressed streams, verified against a test corpus containing corrupt and linearized PDFs.

---

#### Task 2.3: Zero-Copy Byte Lexer & Tokenizer
* **What needs to be changed:** Implement a zero-copy lexical scanner over PDF indirect objects.
* **Why it needs to be changed:** Eliminates heap allocations during PDF syntax parsing.
* **How it should be implemented:**
  1. Create `ZeroCopyLexer` in `include/pdftoolkit/parser/lexer.hpp`.
  2. Output `PdfToken` structures containing `std::string_view` slices pointing into the mmap buffer:
     ```cpp
     struct PdfToken {
         enum class Type {
             Keyword, Integer, Real, StringLit, HexStr,
             Name, DictStart, DictEnd, ArrayStart, ArrayEnd,
             StreamStart, StreamEnd
         } type;
         std::string_view value;
         int64_t int_val;
         float float_val;
     };
     ```
  3. Implement branchless whitespace/comment stripping via a 256-entry lookup table (`alignas(64) static const bool is_ws[256]`).
* **Affected files/modules:** `include/pdftoolkit/parser/lexer.hpp`, `src/parser/lexer.cpp`
* **Dependencies / Prerequisites:** Task 1.1.
* **Verification & Acceptance Criteria:** Tokenization throughput exceeds 2.5 GB/s on uncompressed streams in Google Benchmark suites.

---

#### Task 2.4: Hardware-Accelerated Flate Decompressor (`libdeflater`)
* **What needs to be changed:** Replace general-purpose zlib with a hardware-accelerated Flate decoder.
* **Why it needs to be changed:** Decompressing stream data is the primary compute bottleneck in PDF ingestion.
* **How it should be implemented:**
  1. Integrate `libdeflater` via CMake FetchContent.
  2. Wrap it in `src/codec/flate.cpp`.
  3. Decompress raw streams directly into memory allocated from the thread's `BumpArena`.
  4. Implement early defenses against decompression bombs:
     - Check uncompressed length `/Length1` metadata if present.
     - Hard abort if decompressed bytes exceed $128 \times$ compressed size or the engine's per-page memory budget (e.g., 64 MB).
* **Affected files/modules:** `include/pdftoolkit/codec/flate.hpp`, `src/codec/flate.cpp`
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
  1. Implement `CMapTable` in `src/font/cmap.cpp`.
  2. Parse `/ToUnicode` streams:
     - Handle `beginbfrange` and `beginbfchar` blocks.
     - Map 1-byte and 2-byte character codes to target Unicode codepoints.
  3. Normalize multi-character ligatures: Expand `0xFB01` to `{'f', 'i'}`.
  4. Deduplicate parsed CMaps globally across the engine instance using `xxHash64` content hashing.
* **Affected files/modules:** `include/pdftoolkit/font/cmap.hpp`, `src/font/cmap.cpp`
* **Dependencies / Prerequisites:** Tasks 2.3, 2.4.
* **Verification & Acceptance Criteria:** Accurate decoding of complex CID-keyed and TrueType subset fonts verified against a standardized glyph-test document.

---

#### Task 3.2: Content Stream Operator State Machine
* **What needs to be changed:** Implement a push-down state machine for text-rendering operators.
* **Why it needs to be changed:** Correctly tracks affine transformations, font states, and coordinate vectors.
* **How it should be implemented:**
  1. Implement `OperatorEvaluator` in `src/layout/evaluator.cpp`.
  2. Maintain affine matrices in CPU registers using `std::array<float, 6>`:
     - Text Matrix ($T_m$), Line Matrix ($T_l$).
     - Graphics State Stack (`q`, `Q` operations).
     - Current Transformation Matrix ($CTM$).
  3. Evaluate operators without heap allocation:
     - `BT` / `ET`: Begin/End text object.
     - `Tf`: Set active font and font size.
     - `Td` / `TD`: Move text position.
     - `Tm`: Set text matrix.
     - `Tj` / `'` / `"` / `TJ`: Render text strings with kerning adjustments.
* **Affected files/modules:** `include/pdftoolkit/layout/evaluator.hpp`, `src/layout/evaluator.cpp`
* **Dependencies / Prerequisites:** Tasks 3.1, 2.3.
* **Verification & Acceptance Criteria:** Graphics and text matrix calculations match Adobe Acrobat reference output to within $\pm 0.001$ point precision.

---

#### Task 3.3: Single-Pass Slab Population Engine
* **What needs to be changed:** Stream decoded characters, bounding boxes, and term hashes directly into the `PageSlab`.
* **Why it needs to be changed:** Replaces the 4-pass conversion pipeline with a single forward pass.
* **How it should be implemented:**
  1. Implement `materialize_page_slab` in `src/layout/materializer.cpp`.
  2. As each character code is parsed in `TJ`/`Tj`:
     - Transform coordinates: $\begin{bmatrix} x_{dev} \\ y_{dev} \end{bmatrix} = CTM \times T_m \times \begin{bmatrix} x \\ y \end{bmatrix}$.
     - Resolve character code to Unicode via `CMapTable`.
     - Write $X, Y, W, H$ directly into the slab's contiguous coordinate arrays.
     - Write the codepoint into the glyph attribute array.
  3. Compute word-break boundaries: Emit a 64-bit hash (`xxHash64`) for each completed token into the slab's term hash section.
* **Affected files/modules:** `include/pdftoolkit/layout/materializer.hpp`, `src/layout/materializer.cpp`
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
  1. Implement `InvertedIndex` in `src/index/inverted.cpp`.
  2. Maintain a contiguous Term Lexicon using a Monolithic Radix Array or Compact String Buffer:
     - Store unique terms in a flat byte arena with 32-bit offsets.
  3. For each unique term, maintain a posting list compressed using PForDelta (packing 128 doc/page IDs per block).
  4. Record per-block upper-bound BM25 scores ($U_b$) for Block-Max WAND evaluation.
* **Affected files/modules:** `include/pdftoolkit/index/inverted.hpp`, `src/index/inverted.cpp`
* **Dependencies / Prerequisites:** Task 3.3.
* **Verification & Acceptance Criteria:** Indexing 10,000 pages takes $< 1.0$ second; posting list memory footprint is $< 1.5$ bytes per indexed token.

---

#### Task 4.2: Block-Max WAND Query Engine
* **What needs to be changed:** Implement early-termination search pruning.
* **Why it needs to be changed:** Avoids calculating scores for 90%+ of non-competitive documents.
* **How it should be implemented:**
  1. Implement `BlockMaxWandIterator` in `src/search/wand.cpp`.
  2. Algorithm:
     - Maintain a min-heap of the top-$K$ scoring results.
     - Sort posting iterators by current document ID.
     - Sum the maximum BM25 contribution scores ($U_b$) across terms for the current block.
     - If $\sum U_b \le \text{heap.min()}$, advance the iterator past the entire block without calculating individual document scores.
* **Affected files/modules:** `include/pdftoolkit/search/wand.hpp`, `src/search/wand.cpp`
* **Dependencies / Prerequisites:** Task 4.1.
* **Verification & Acceptance Criteria:** Query benchmark demonstrates a 15x–40x speedup over brute-force scoring on multi-term queries.

---

#### Task 4.3: AVX2/NEON Direct Needle Vector Search
* **What needs to be changed:** Implement hardware-accelerated substring search for unindexed memory buffers.
* **Why it needs to be changed:** Provides fast fallbacks for ad-hoc search without index build overhead.
* **How it should be implemented:**
  1. Implement `simd_search_bytes` in `src/search/simd.cpp`.
  2. For x86_64: Load 32 bytes into `__m256i`, broadcast the needle's first character, execute `_mm256_cmpeq_epi8`, and extract the bitmask using `_mm256_movemask_epi8`.
  3. For AArch64: Load 16 bytes into `uint8x16_t`, execute `vceqq_u8`, and compress the mask using `vshrq_n_u8`.
  4. Provide a scalar fallback to preserve cross-platform support.
* **Affected files/modules:** `include/pdftoolkit/search/simd.hpp`, `src/search/simd.cpp`
* **Dependencies / Prerequisites:** Task 1.1.
* **Verification & Acceptance Criteria:** Single-thread scan speed exceeds 4.0 GB/s on raw text buffers.

---

#### Task 4.4: SIMD Spatial Bounding-Box Filter
* **What needs to be changed:** Replace linear coordinate loops with vectorized spatial window filters.
* **Why it needs to be changed:** Enables instant extraction of text within vertical/horizontal context windows.
* **How it should be implemented:**
  1. Implement `spatial_window_filter` in `src/geometry/spatial.cpp`.
  2. Load 8 consecutive $Y$-coordinates from the slab into an AVX2 vector (`_mm256_load_ps`).
  3. Compare against bounds $[Y_{min}, Y_{max}]$ using `_mm256_cmp_ps`.
  4. Use the resulting bitmask to select matching glyph indices without branch mispredictions.
* **Affected files/modules:** `include/pdftoolkit/geometry/spatial.hpp`, `src/geometry/spatial.cpp`
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
  1. Implement `PagePlan` in `include/pdftoolkit/ops/plan.hpp`:
     ```cpp
     struct VirtualPageRef {
         uint32_t source_doc_id;
         uint32_t source_page_idx;
         uint32_t object_id;
     };

     class PagePlan {
     public:
         std::vector<VirtualPageRef> pages;
         static std::expected<PagePlan, ErrorCode> parse(std::string_view spec, uint32_t max_pages);
     };
     ```
  2. Implement a strict, zero-allocation page specification parser:
     - Supports syntax: `"1-5,10,12-end"`, `"all"`, `"none"`.
     - Rejects out-of-bounds, inverted, or malformed specs with typed errors (`ErrorCode::InvalidPageRange`).
* **Affected files/modules:** `include/pdftoolkit/ops/plan.hpp`, `src/ops/plan.cpp`
* **Dependencies / Prerequisites:** Task 2.2.
* **Verification & Acceptance Criteria:** Plan compilation executes in $< 10$ microseconds and performs zero allocations on document streams.

---

#### Task 5.2: Zero-Copy Incremental PDF Recompiler & Streaming Writer
* **What needs to be changed:** Replace PyMuPDF document serialization with a native zero-copy streaming writer.
* **Why it needs to be changed:** Enables instant document merges and exports by streaming indirect object bytes without re-encoding.
* **How it should be implemented:**
  1. Implement `StreamingPdfWriter` in `src/ops/writer.cpp`.
  2. Steps:
     - Construct a balanced, minimal `/Pages` dictionary tree.
     - Re-number object IDs sequentially ($1 \dots M$).
     - Stream raw indirect object byte slices directly from the source `MmapHandle` to the output sink using vectorized writes (`writev` / POSIX I/O).
     - Write a compact cross-reference table and trailer dictionary.
  3. Enforce the **Overwriting Invariant**: Verify that the destination path does not collide with any active input file descriptor, returning `ErrorCode::OutputConflict` if a collision is detected.
* **Affected files/modules:** `include/pdftoolkit/ops/writer.hpp`, `src/ops/writer.cpp`
* **Dependencies / Prerequisites:** Tasks 5.1, 1.1.
* **Verification & Acceptance Criteria:** Merging two 500-page documents executes in $< 15$ milliseconds, dominated entirely by disk I/O.

---

### Phase 6: Multi-Core Concurrency & Any-Time Execution Scheduler (Layer 5)

```
REQUEST ──► [Lock-Free Registry] ──► [Work-Stealing Pool]
                                            │
                                            ▼
                              [SearchWatchdog: Timeout Check]
                                            │
                                            ▼
                              Any-Time Partial Top-K Results
```

#### Task 6.1: Lock-Free Atomic Document Registry
* **What needs to be changed:** Replace Python's `threading.RLock()` and dictionary registry with a concurrent, lock-free registry.
* **Why it needs to be changed:** Eliminates thread contention, allowing thousands of concurrent reads without blocking document additions or removals.
* **How it should be implemented:**
  1. Implement `DocRegistry` in `src/runtime/registry.cpp`.
  2. Use lock-free epoch management or Hazard Pointers:
     - Read operations acquire an atomic reference and read document metadata without locking.
     - Document removal marks entries as retired; physical unmapping (`munmap`) is deferred until all concurrent reader references drop to zero.
  3. Assign documents stable 32-bit IDs (`uint32_t`).
* **Affected files/modules:** `include/pdftoolkit/runtime/registry.hpp`, `src/runtime/registry.cpp`
* **Dependencies / Prerequisites:** Task 1.1.
* **Verification & Acceptance Criteria:** Benchmark showing 32 concurrent threads executing continuous lookups and deregistrations with zero deadlocks and zero latency spikes.

---

#### Task 6.2: Work-Stealing Execution Pool
* **What needs to be changed:** Replace single-threaded sequential corpus scans with work-stealing parallel processing.
* **Why it needs to be changed:** Maximizes multi-core CPU utilization during large-scale document operations.
* **How it should be implemented:**
  1. Implement `ExecutionPool` in `src/runtime/pool.cpp` using a custom work-stealing deque or Intel oneTBB.
  2. Implement adaptive batching for corpus file scans: Chunk document lists dynamically to prevent worker starvation when documents have varying page counts.
  3. Pin worker threads to physical CPU cores using `pthread_setaffinity_np` (Linux) or `SetThreadAffinityMask` (Windows) to preserve L1/L2 cache locality.
* **Affected files/modules:** `include/pdftoolkit/runtime/pool.hpp`, `src/runtime/pool.cpp`
* **Dependencies / Prerequisites:** Task 6.1.
* **Verification & Acceptance Criteria:** Processing a directory of 1,000 PDFs scales linearly from 1 to 16 threads ($\ge 14\times$ throughput speedup).

---

#### Task 6.3: Any-Time Execution Scheduler (`SearchWatchdog`)
* **What needs to be changed:** Prevent runaway operations from violating latency budgets.
* **Why it needs to be changed:** Guarantees strict P99 latency bounds under concurrent load.
* **How it should be implemented:**
  1. Implement `SearchWatchdog` in `include/pdftoolkit/runtime/watchdog.hpp`:
     ```cpp
     class SearchWatchdog {
     private:
         std::chrono::steady_clock::time_point deadline_;
         std::atomic<bool>& cancel_flag_;
     public:
         SearchWatchdog(std::chrono::milliseconds timeout, std::atomic<bool>& cancel)
             : deadline_(std::chrono::steady_clock::now() + timeout), cancel_flag_(cancel) {}

         [[nodiscard]] inline bool check(size_t step) const noexcept {
             if ((step & 0x7F) == 0) { // Check clock every 128 iterations to minimize syscalls
                 if (cancel_flag_.load(std::memory_order_relaxed) ||
                     std::chrono::steady_clock::now() >= deadline_) {
                     return false; // Abort signal
                 }
             }
             return true;
         }
     };
     ```
  2. Integrate the check into the inner loops of the WAND algorithm and corpus traversals.
  3. Upon timeout, stop further exploration and return the top-$K$ scoring candidates found up to that point.
* **Affected files/modules:** `include/pdftoolkit/runtime/watchdog.hpp`
* **Dependencies / Prerequisites:** Tasks 4.2, 6.2.
* **Verification & Acceptance Criteria:** Setting a 10 ms deadline on a query that normally takes 500 ms returns valid partial results in $\le 10.5$ ms.

---

### Phase 7: Zero-Copy Interfaces, C-FFI, & Buffer Protocol Bindings (Layer 6)

```
NATIVE CORE ──► [pdftoolkit-ffi] (C-ABI pdftoolkit.h)
                       │
                       ├─► [pdftoolkit-py] ──► Python NumPy Buffer Protocol (Zero-Copy)
                       │
                       └─► [pdftoolkit-cli] ──► Raw Arrow/JSON Stream to stdout
```

#### Task 7.1: Panic-Proof C-ABI Export Layer (`pdftoolkit-ffi`)
* **What needs to be changed:** Export clean, unboxed C-ABI symbols without allowing C++ exceptions to cross the FFI boundary.
* **Why it needs to be changed:** Provides an interface for non-C++ runtimes (Python, Go, C, Zig).
* **How it should be implemented:**
  1. Export C-ABI functions in `include/pdftoolkit/pdftoolkit.h` and `src/ffi/pdftoolkit.cpp`:
     ```cpp
     extern "C" {
     int32_t pdftoolkit_engine_create(EngineHandle** out_handle);
     int32_t pdftoolkit_engine_destroy(EngineHandle* handle);
     int32_t pdftoolkit_register_document(EngineHandle* handle, const char* path, uint32_t* out_doc_id);
     int32_t pdftoolkit_search_wand(
         EngineHandle* handle,
         const char* query,
         uint32_t top_k,
         uint64_t timeout_ms,
         FfiResultSlice* out_slice
     );
     }
     ```
  2. Wrap all extern functions in `try ... catch (...)` mapping C++ exceptions to integer error codes.
* **Affected files/modules:** `include/pdftoolkit/pdftoolkit.h`, `src/ffi/pdftoolkit.cpp`
* **Dependencies / Prerequisites:** Phases 1–6.
* **Verification & Acceptance Criteria:** C integration test compiles with `gcc -Wall -Wextra -Werror` and runs queries without memory leaks.

---

#### Task 7.2: High-Performance Pybind11 / Python C-API Native Extension (`pdftoolkit-py`)
* **What needs to be changed:** Re-implement the Python bindings to use zero-copy memory buffers.
* **Why it needs to be changed:** Allows Python to access extracted coordinates and text without copying data into Python-managed heap strings.
* **How it should be implemented:**
  1. Implement `src/python/bindings.cpp` using `pybind11`.
  2. Implement Python's **Buffer Protocol** (`Py_buffer` / `pybind11::buffer_info`):
     - Expose the slab's $X, Y, W, H$ float coordinates directly to Python as contiguous 2D NumPy array views (`np.frombuffer`).
  3. Release the Python Global Interpreter Lock on all processing calls:
     ```cpp
     py::class_<Engine>(m, "Engine")
         .def("search", [](Engine& self, const std::string& query) {
             py::gil_scoped_release release;
             return self.search_internal(query);
         });
     ```
* **Affected files/modules:** `src/python/bindings.cpp`, `src/pdftoolkit/__init__.py`
* **Dependencies / Prerequisites:** Task 7.1.
* **Verification & Acceptance Criteria:** Python integration tests verify that NumPy operations on extracted layout arrays complete with zero underlying memory copies.

---

#### Task 7.3: Native Headless CLI Binary (`pdftoolkit-cli`)
* **What needs to be changed:** Build a compiled native CLI binary replacing `python -m pdftoolkit`.
* **Why it needs to be changed:** Eliminates Python runtime startup overhead (reducing invocation latency from 150 ms to $< 3$ ms).
* **How it should be implemented:**
  1. Build the command-line interface in `src/cli/main.cpp` using `CLI11`.
  2. Implement subcommands: `search`, `corpus-scan`, `slice`, `merge`, `inspect`.
  3. Output structured results directly to `stdout` as Apache Arrow IPC streams or JSON lines.
* **Affected files/modules:** `src/cli/main.cpp`
* **Dependencies / Prerequisites:** Task 7.1.
* **Verification & Acceptance Criteria:** CLI execution latency (`--help` or simple info commands) runs in $< 5$ milliseconds end-to-end.

---

### Phase 8: Adversarial Fuzzing, Numerical Verification, & Production Benchmarking

#### Task 8.1: Continuous LLVM LibFuzzer Harness
* **What needs to be changed:** Set up continuous automated fuzz testing against engine entry points.
* **Why it needs to be changed:** Verifies that malformed, corrupted, or malicious PDFs cannot crash the engine or trigger undefined behavior.
* **How it should be implemented:**
  1. Implement fuzz targets under `fuzz/`:
     - `fuzz_lexer.cpp`: Feeds arbitrary byte streams to the zero-copy tokenizer.
     - `fuzz_xref.cpp`: Feeds mutated offsets to the XRef table parser.
     - `fuzz_evaluator.cpp`: Feeds corrupted graphics operators to the state machine.
  2. Compile with `-fsanitize=fuzzer,address,undefined`.
  3. Run 50,000,000 fuzz cycles without crashes.
* **Affected files/modules:** `fuzz/*`
* **Dependencies / Prerequisites:** Phases 1–3.
* **Verification & Acceptance Criteria:** Zero crashes, hangs, or memory leaks across 50,000,000 executions.

---

#### Task 8.2: Numerical Stability & Invariance Test Suite
* **What needs to be changed:** Build deterministic mathematical verification tests for scoring and layout operations.
* **Why it needs to be changed:** Ensures identical calculation output across target architectures (x86_64 vs. AArch64).
* **How it should be implemented:**
  1. Create numerical regression tests in `tests/numerical_test.cpp`:
     - Verify BM25 score stability across floating-point implementations.
     - Verify layout coordinate transforms against reference matrices.
  2. Create edge-case regression tests for:
     - 0-page empty PDFs.
     - Broken xref offsets requiring fallback scans.
     - Massive documents ($\ge 50,000$ pages).
     - Circular object reference loops (preventing infinite recursion).
* **Affected files/modules:** `tests/*`
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

The overhaul is complete only when the new C++20 engine passes these quantitative acceptance gates:

| Metric | Previous Python Wrapper (`v2.0.0`) | Native C++20 Systems Engine Target | Verification Tool |
|---|---|---|---|
| **Corpus Scan (10,000 Pages)** | 14.8 seconds | **$\le 0.18$ seconds ($> 80\times$ faster)** | `google/benchmark` / `benchmarks/run_suite.py` |
| **Peak Memory (10k Page Index)** | 820 MB (Python Heap Strings) | **$\le 20$ MB (Mmap + Page Slabs)** | Linux `/usr/bin/time -v` (Peak RSS) |
| **P99 Query Latency** | 350 ms | **$\le 1.2$ ms** | Google Benchmark P99 Measurement |
| **Multi-Core Scaling (32 Threads)** | Locked by GIL (1 core max) | **$\ge 28\times$ linear speedup** | `bench_concurrency_scaling` |
| **L1 Data Cache Miss Rate** | $\ge 18\%$ | **$\le 1.8\%$** | Linux `perf stat -e L1-dcache-load-misses` |
| **IPC (Instructions Per Cycle)** | $\le 0.8$ (DRAM stalled) | **$\ge 2.9$** | Linux `perf stat -e instructions,cycles` |
| **Binary Invocation Latency** | 150 ms (Python runtime start) | **$\le 3$ ms** | `hyperfine --warmup 5 'pdftoolkit-cli'` |
| **Crash Rate on Malformed PDFs** | Handled via Python exceptions | **0 crashes across $5\times 10^7$ fuzz iterations** | LLVM LibFuzzer (`-fsanitize=fuzzer,address`) |

---

## 5. Architectural Closure

This plan closes the gap between theory and code without using Rust:
1. **Memory is managed directly:** Virtual memory slabs (`PageSlab`) and thread bump arenas replace dynamic heap allocations.
2. **Work is eliminated algorithmically:** Block-Max WAND and PForDelta posting lists replace linear regex scans.
3. **Hardware capabilities are used:** C++20 `alignas(64)` and AVX2/NEON vector intrinsics maximize cache usage.
4. **Execution is bounded:** Microsecond any-time deadlines prevent long queries from monopolizing the engine.
5. **Language boundaries are zero-copy:** The Python Buffer Protocol (`Py_buffer`) allows NumPy and Python consumers to read C++ engine memory directly without copies.

Executing this plan transforms the repository into an industrial C++20 systems engine that will stand up to the most demanding systems engineering scrutiny.