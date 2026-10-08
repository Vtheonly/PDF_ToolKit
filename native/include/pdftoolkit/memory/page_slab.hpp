#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "pdftoolkit/errors.hpp"
#include "pdftoolkit/memory/arena.hpp"

namespace pdftoolkit::memory {

/// Unified Page Slab (UPS) binary layout (audit issue-1/task 1.3).
///
/// One page = one contiguous, single-allocation, unboxed record living
/// in a BumpArena: a 64-byte header (exactly one cache line) followed by
/// the SoA coordinate arrays, the UTF-32 glyph stream, the sorted term
/// hashes and the opaque postings payload.
///
/// Layout (offsets relative to the slab start, every section 32-byte
/// aligned — the audit requires it for the four coordinate arrays, the
/// others follow the same rule for a uniform invariant):
///
///   [PageSlabHeader, 64 B]
///   [pad to 32] [x: float  x glyph_count]
///   [pad to 32] [y: float  x glyph_count]
///   [pad to 32] [w: float  x glyph_count]
///   [pad to 32] [h: float  x glyph_count]
///   [pad to 32] [codepoints: uint32 x glyph_count]
///   [pad to 32] [term_hashes: uint64 x term_count, sorted]
///   [pad to 32] [postings: opaque bytes (typed by task 4.1)]
///
/// Format properties:
///   * native endianness, format version "UPS1" (magic);
///   * offsets are uint32 — a slab is at most 4 GiB;
///   * crc32 (IEEE 802.3, reflected 0xEDB88320) over the whole slab with
///     the crc32 field itself excluded;
///   * `reserved` is zero on output, ignored on input.
struct alignas(64) PageSlabHeader {
    std::uint8_t  magic[4];           // "UPS1"
    std::uint32_t page_index;
    std::uint32_t glyph_count;
    std::uint32_t term_count;
    float         width_pt;
    float         height_pt;
    std::uint32_t offset_x;           // AVX2 32-byte aligned offset
    std::uint32_t offset_y;           // AVX2 32-byte aligned offset
    std::uint32_t offset_w;           // AVX2 32-byte aligned offset
    std::uint32_t offset_h;           // AVX2 32-byte aligned offset
    std::uint32_t offset_codepoints;  // Offset to UTF-32 glyphs
    std::uint32_t offset_term_hashes; // Offset to sorted 64-bit term hashes
    std::uint32_t offset_postings;    // Offset to in-page posting entries
    std::uint32_t crc32;
    // NOTE (P-015): the audit specified reserved[12], which makes the
    // struct 68 bytes — and alignas(64) would round it to 128, so the
    // audit's own static_assert(sizeof == 64) could never pass. The
    // padding is corrected to 8 bytes; every named field is kept.
    std::uint8_t  reserved[8];
};
static_assert(sizeof(PageSlabHeader) == 64,
              "Header must occupy exactly one cache line");
// Binary-format stability: these are positions in the on-arena format,
// frozen by the "UPS1" magic version.
static_assert(offsetof(PageSlabHeader, magic) == 0);
static_assert(offsetof(PageSlabHeader, offset_x) == 24);
static_assert(offsetof(PageSlabHeader, offset_postings) == 48);
static_assert(offsetof(PageSlabHeader, crc32) == 52);
static_assert(offsetof(PageSlabHeader, reserved) == 56);

/// The slab's alignment requirement, as a number (alignof of the header).
inline constexpr std::size_t kPageSlabAlignment = alignof(PageSlabHeader);

/// Every section offset inside a slab is a multiple of this (the audit's
/// AVX2 requirement, applied uniformly).
inline constexpr std::uint32_t kSectionAlignment = 32;

/// Builder input: the page's unboxed data, borrowed from the caller.
/// `x/y/w/h/codepoints` must all have `glyph_count` elements;
/// `term_hashes` must be sorted ascending.
struct PageSlabSource {
    std::uint32_t page_index = 0;
    float width_pt = 0.0F;
    float height_pt = 0.0F;
    std::span<const float> x;
    std::span<const float> y;
    std::span<const float> w;
    std::span<const float> h;
    std::span<const std::uint32_t> codepoints;    // UTF-32, one per glyph
    std::span<const std::uint64_t> term_hashes;   // sorted, unique
    std::span<const std::uint8_t> postings;       // opaque until task 4.1
};

/// Builds one slab for `source` inside `arena` and returns the raw slab
/// bytes (empty span when the arena is exhausted — the arena's own
/// convention; no partial slab is ever left behind).
///
/// Throws PdfToolkitException(InvalidArgument) for malformed sources
/// (mismatched array sizes, unsorted term hashes, counts or total size
/// beyond the uint32 format limits).
///
/// Allocation note: the arena hands out slices aligned to alignof(T) of
/// the requested element type, so a 64-aligned slab is obtained by
/// allocating through an alignas(64) one-byte storage unit (the arena
/// component stays untouched; this adapter is the slab module's own
/// concern).
[[nodiscard]] std::span<std::uint8_t> build_page_slab(
    BumpArena& arena, const PageSlabSource& source);

/// Zero-copy read view over slab bytes (audit task 1.3 step 2).
///
/// Construction validates magic, section alignment, bounds and counts;
/// violations throw PdfToolkitException(InvalidArgument). All accessors
/// return spans pointing INTO the slab — no copies, no ownership.
class PageSlabView {
public:
    explicit PageSlabView(std::span<const std::uint8_t> slab);

    [[nodiscard]] const PageSlabHeader& header() const noexcept {
        return header_;
    }
    [[nodiscard]] std::uint32_t page_index() const noexcept {
        return header_.page_index;
    }
    [[nodiscard]] std::uint32_t glyph_count() const noexcept {
        return header_.glyph_count;
    }
    [[nodiscard]] std::uint32_t term_count() const noexcept {
        return header_.term_count;
    }

    /// SoA coordinate arrays (glyph_count floats each, 32-byte aligned).
    [[nodiscard]] std::span<const float> x() const noexcept { return x_; }
    [[nodiscard]] std::span<const float> y() const noexcept { return y_; }
    [[nodiscard]] std::span<const float> w() const noexcept { return w_; }
    [[nodiscard]] std::span<const float> h() const noexcept { return h_; }

    /// UTF-32 glyph stream (glyph_count code points).
    [[nodiscard]] std::span<const std::uint32_t> codepoints() const noexcept {
        return codepoints_;
    }
    /// Sorted 64-bit term hashes (term_count).
    [[nodiscard]] std::span<const std::uint64_t> term_hashes() const noexcept {
        return term_hashes_;
    }
    /// Opaque postings payload; the typed accessor arrives with the
    /// in-page posting format (audit task 4.1).
    [[nodiscard]] std::span<const std::uint8_t> postings() const noexcept {
        return postings_;
    }

    /// Recomputes the CRC over the slab and compares it with the
    /// header's crc32 field.
    [[nodiscard]] bool verify_crc() const noexcept;

private:
    const std::uint8_t* base_;
    std::size_t size_;
    PageSlabHeader header_{};
    std::span<const float> x_, y_, w_, h_;
    std::span<const std::uint32_t> codepoints_;
    std::span<const std::uint64_t> term_hashes_;
    std::span<const std::uint8_t> postings_;
};

/// IEEE 802.3 CRC-32 (reflected, poly 0xEDB88320, init/xorout 0xFFFFFFFF)
/// over `bytes`. The crc of an empty range is 0, per the standard.
/// Exposed for tests and for future format tooling; the slab's two-range
/// chaining (crc field excluded) is internal to the .cpp.
[[nodiscard]] std::uint32_t crc32_ieee(
    std::span<const std::uint8_t> bytes) noexcept;

}  // namespace pdftoolkit::memory
