#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "pdftoolkit/codec/flate.hpp"
#include "pdftoolkit/memory/arena.hpp"

namespace pdftoolkit::font {

/// Parse statistics for one /ToUnicode CMap (diagnostics + evidence;
/// zero mappings is a legal, honestly-reported outcome — some embedded
/// CMaps are pure boilerplate).
struct CMapStats {
    /// beginbfchar entries accepted into the table.
    std::size_t bfchar_entries = 0;
    /// beginbfrange entries accepted (each range start counts once;
    /// array-form elements count individually).
    std::size_t bfrange_entries = 0;
    /// Distinct character codes mapped after last-wins merging.
    std::size_t mapped_codes = 0;
    /// Malformed/hostile entries skipped (bad hex, lone surrogates,
    /// out-of-scope codes, inverted ranges, ...). Never fatal.
    std::size_t skipped_entries = 0;
    /// Character-code width in bytes (1 or 2) as declared by
    /// codespacerange (or inferred from the widest mapped source when
    /// absent). 0 when the CMap maps nothing and declares nothing.
    unsigned code_bytes = 0;
    /// A codespacerange block was seen.
    bool saw_codespace = false;
    /// At least one bfchar/bfrange block was seen.
    bool saw_bf_blocks = false;
};

/// Immutable mapping from PDF character codes (1- or 2-byte, the
/// audit's task-3.1 scope) to UTF-32 codepoint sequences (audit task
/// 3.1). Built once by `parse`, then frozen and shared — lookups are
/// const and thread-safe by construction (no mutable state).
///
/// Semantics:
///   * sources: beginbfchar pairs and beginbfrange blocks (both the
///     3-operand contiguous form and the explicit array form);
///   * destinations: UTF-16BE hex strings — surrogate pairs decode to
///     astral codepoints, and the presentation-form LIGATURES are
///     expanded AT BUILD TIME (U+FB01 -> {'f','i'} per the audit; the
///     full verified table lives in cmap.cpp), so `lookup` results are
///     final — the task-3.3 hot path copies, never re-normalizes;
///   * later entries overwrite earlier ones (PDF definition order);
///     malformed entries are skipped and counted, never fatal (the
///     tolerant-parser contract of tasks 2.1-2.3);
///   * codes above 0xFFFF (the audit scopes this table to 1-2-byte
///     codes) and ranges that would overflow 0x10FFFF or the 65,536-
///     entry direct index are refused entry-by-entry — a hostile CMap
///     cannot size the table;
///   * `lookup` returns an EMPTY span for unmapped codes (including
///     codes beyond the table); mapping to an empty destination is
///     deliberately indistinguishable (documented).
class CMapTable {
public:
    /// Parses an UNCOMPRESSED /ToUnicode CMap program. Returns a
    /// frozen table (never null; a garbage input yields an honest
    /// zero-mapping table — check `stats`). Allocation failures throw
    /// std::bad_alloc; nothing else throws.
    [[nodiscard]] static std::shared_ptr<const CMapTable> parse(
        std::span<const std::uint8_t> cmap_text, CMapStats* stats = nullptr);

    /// Resolves one character code to its UTF-32 codepoint sequence
    /// (zero-copy: the span points into the table's own pool and lives
    /// as long as the table). Empty span = unmapped. Never throws,
    /// never reads out of bounds — the O(1) direct index makes hostile
    /// codes a single bounds check.
    [[nodiscard]] std::span<const char32_t> lookup(
        std::uint32_t code) const noexcept;

    /// Distinct mapped codes (post last-wins merge).
    [[nodiscard]] std::size_t mapped_codes() const noexcept {
        return mapped_codes_;
    }

    /// Declared/inferred code width in bytes (1 or 2; 0 = nothing
    /// mapped and no codespace). Task 3.2's text-string walker needs
    /// this to split byte streams into codes.
    [[nodiscard]] unsigned code_bytes() const noexcept {
        return code_bytes_;
    }

    CMapTable() = default;
    ~CMapTable() = default;
    CMapTable(const CMapTable&) = delete;
    CMapTable& operator=(const CMapTable&) = delete;

private:
    struct Entry {
        std::uint32_t offset;  // into pool_
        std::uint32_t length;
    };

    // Direct index: code -> entry id + 1 (0 = unmapped). Sized to the
    // highest mapped code + 1 (<= 65,537 entries by construction).
    std::vector<std::uint32_t> index_;
    std::vector<Entry> entries_;
    std::vector<char32_t> pool_;
    std::size_t mapped_codes_ = 0;
    unsigned code_bytes_ = 0;
};

/// Why an `intern` call did or did not produce a table.
enum class CMapStatus : std::uint8_t {
    /// Parsed (possibly zero mappings — see CMapStats).
    Ok,
    /// The bytes are not an `N G obj <<...>> stream` object.
    BadObject,
    /// A filter pipeline this engine does not decode (non-Flate filter,
    /// multi-filter, or predictor-bearing /DecodeParms — the P-020
    /// class of honest degradation).
    UnsupportedFilter,
    /// Flate stream whose compressed extent needs an indirect /Length
    /// (unresolvable before an object table exists — P-021).
    IndirectLength,
    /// The codec refused or failed on the payload (corrupt, bomb
    /// defense, scratch exhaustion).
    FlateFailed,
    /// Built with PDTK_ENABLE_FLATE=OFF: no codec exists (ADR-0007).
    FlateUnavailable,
};

/// Report for one CMapCache::intern_stream_object call.
struct CMapInternReport {
    CMapStatus status = CMapStatus::Ok;
    CMapStats parse_stats;          // valid for status == Ok
    bool cache_hit = false;         // an identical payload was interned
                                   // before (table shared, not reparsed)
    /// Stable diagnostic (never null).
    const char* message = "";
};

/// Engine-instance-global immutable CMap cache (audit task 3.1 step 4):
/// parses /ToUnicode stream objects once and deduplicates them by
/// xxHash64 CONTENT hashing — fonts across documents that embed the
/// same CMap bytes share one frozen CMapTable.
///
/// Dedup key: xxHash64 (seeded per payload class, so raw and Flate
/// payloads can never collide-merge) over the stream PAYLOAD exactly
/// as it appears in the file — object numbers, generation numbers and
/// /Length spellings do not participate (a producer's boilerplate CMap
/// at object 12 in one file and object 47 in another merges). A hash
/// hit is confirmed by byte comparison before sharing (entries are
/// exact; collisions cannot merge different CMaps).
///
/// Flate handling: the cache owns one decompressor and one lazily
/// created 8 MiB scratch arena (ToUnicode streams are KB-scale; the
/// codec's bomb defenses still apply). The decompressed bytes live
/// only for the parse — CMapTable copies what it keeps, and the arena
/// is rewound after every intern.
///
/// Thread safety: one mutex guards the map (and the codec/arena use
/// under it) — concurrent interns serialize but never race. Phase 6's
/// concurrency story may hand each worker its own cache over a shared
/// map; that is a deliberate future decision, not a silent drift.
class CMapCache {
public:
    CMapCache() = default;
    ~CMapCache() = default;
    CMapCache(const CMapCache&) = delete;
    CMapCache& operator=(const CMapCache&) = delete;

    /// Interns the /ToUnicode stream OBJECT at `object_bytes` — the
    /// full `N G obj << /Length .. /Filter .. >> stream ... endstream`
    /// range as it appears in the file (the caller resolves the offset
    /// via the task-2.2 XRefIndex; following /Font /ToUnicode N 0 R
    /// references through the object graph is a later, still
    /// unassigned task — see the problem registry).
    ///
    /// Returns the shared immutable table, or nullptr with the reason
    /// in `report` (never throws except std::bad_alloc).
    [[nodiscard]] std::shared_ptr<const CMapTable> intern_stream_object(
        std::span<const std::uint8_t> object_bytes,
        CMapInternReport* report = nullptr);

    /// Distinct CMaps currently held.
    [[nodiscard]] std::size_t size() const;

    /// Total intern calls that reused an existing table (diagnostics).
    [[nodiscard]] std::size_t intern_hits() const;

private:
    struct Entry {
        std::uint64_t hash;                    // payload xxHash64
        std::vector<std::uint8_t> payload;     // exact-compare copy
        std::shared_ptr<const CMapTable> table;
    };

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;  // linear over distinct payloads; CMap
                                  // counts per corpus are small (tens),
                                  // and the hash bucket prefix keeps
                                  // collision chains short — see the
                                  // implementation note in cmap.cpp
    std::size_t intern_hits_ = 0;
    codec::FlateDecompressor decompressor_;
    std::optional<memory::BumpArena> scratch_;
};

}  // namespace pdftoolkit::font
