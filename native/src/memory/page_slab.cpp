// Unified Page Slab — builder and zero-copy view (audit issue-1/task 1.3).
//
// The slab is a single contiguous record inside a BumpArena:
// [64-byte header][x][y][w][h][codepoints][term_hashes][postings], every
// section 32-byte aligned (uniform invariant; the coordinate arrays
// require it for AVX2 — the audit's acceptance criterion). Offsets are
// uint32, relative to the slab start; the format is native-endian,
// version-tagged by the "UPS1" magic and pinned by a CRC-32 over the
// slab bytes with the crc32 field itself excluded (so the exact byte
// length is part of the protected value).

#include "pdftoolkit/memory/page_slab.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace pdftoolkit::memory {

namespace {

constexpr std::uint8_t kUps1Magic[4] = {'U', 'P', 'S', '1'};

constexpr auto kCrcOffset = offsetof(PageSlabHeader, crc32);

// Storage adapter: BumpArena serves slices aligned to alignof(T) of the
// requested element, so one 64-aligned slab allocation is expressed as
// an array of 64-byte units. sizeof(SlabUnit) == 64 == alignof(SlabUnit),
// satisfying the arena's compile-time alignment cap.
struct alignas(64) SlabUnit {
    std::uint8_t byte;
};
static_assert(sizeof(SlabUnit) == 64 && alignof(SlabUnit) == 64);

std::uint64_t round_up_to(std::uint64_t value, std::uint64_t alignment) {
    return (value + (alignment - 1)) & ~(alignment - 1);
}

// Raw CRC state update (reflected, poly 0xEDB88320). `crc` is the
// running state; the finalized value is the complement.
std::uint32_t crc32_update(std::span<const std::uint8_t> bytes,
                           std::uint32_t crc) noexcept {
    for (const std::uint8_t b : bytes) {
        crc ^= b;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

// CRC over the whole slab with the crc32 field skipped; `size` must be
// at least sizeof(PageSlabHeader).
std::uint32_t slab_crc(const std::uint8_t* base, std::size_t size) noexcept {
    std::uint32_t crc = crc32_update({base, kCrcOffset}, 0xFFFFFFFFu);
    crc = crc32_update({base + kCrcOffset + sizeof(std::uint32_t),
                        size - kCrcOffset - sizeof(std::uint32_t)},
                       crc);
    return ~crc;
}

// Section placement: the running end cursor, rounded up to the 32-byte
// section alignment before each placement (uniform invariant — the
// coordinate arrays need it for AVX2, the rest follow the same rule).
struct Cursor {
    std::uint64_t end = sizeof(PageSlabHeader);  // header occupies [0,64)

    std::uint32_t place(std::uint64_t bytes) {
        const std::uint64_t at = round_up_to(end, kSectionAlignment);
        end = at + bytes;
        return static_cast<std::uint32_t>(at);
    }
};

}  // namespace

std::uint32_t crc32_ieee(std::span<const std::uint8_t> bytes) noexcept {
    return ~crc32_update(bytes, 0xFFFFFFFFu);
}

std::span<std::uint8_t> build_page_slab(BumpArena& arena,
                                        const PageSlabSource& source) {
    const std::size_t glyphs = source.x.size();
    if (source.y.size() != glyphs || source.w.size() != glyphs ||
        source.h.size() != glyphs || source.codepoints.size() != glyphs) {
        throw PdfToolkitException(
            ErrorCode::InvalidArgument,
            "coordinate and codepoint arrays must all have glyph_count elements");
    }
    if (!std::is_sorted(source.term_hashes.begin(),
                        source.term_hashes.end())) {
        throw PdfToolkitException(ErrorCode::InvalidArgument,
                                  "term hashes must be sorted ascending");
    }
    if (glyphs > std::numeric_limits<std::uint32_t>::max() ||
        source.term_hashes.size() >
            std::numeric_limits<std::uint32_t>::max()) {
        throw PdfToolkitException(
            ErrorCode::InvalidArgument,
            "counts exceed the uint32 slab format");
    }

    Cursor cursor;
    PageSlabHeader header{};
    header.offset_x = cursor.place(glyphs * sizeof(float));
    header.offset_y = cursor.place(glyphs * sizeof(float));
    header.offset_w = cursor.place(glyphs * sizeof(float));
    header.offset_h = cursor.place(glyphs * sizeof(float));
    header.offset_codepoints =
        cursor.place(glyphs * sizeof(std::uint32_t));
    header.offset_term_hashes =
        cursor.place(source.term_hashes.size() * sizeof(std::uint64_t));
    header.offset_postings = cursor.place(source.postings.size());
    const std::uint64_t total = round_up_to(cursor.end, sizeof(SlabUnit));
    if (total > std::numeric_limits<std::uint32_t>::max()) {
        throw PdfToolkitException(
            ErrorCode::InvalidArgument,
            "slab exceeds the 4 GiB uint32 format limit");
    }

    // One 64-aligned allocation for the whole slab.
    const auto units = static_cast<std::size_t>(total / sizeof(SlabUnit));
    const auto storage = arena.alloc_slice<SlabUnit>(units);
    if (storage.empty()) {
        return {};  // arena exhausted — its convention; nothing was written
    }
    std::uint8_t* const base =
        reinterpret_cast<std::uint8_t*>(storage.data());
    // Zero-fill first: deterministic padding + zeroed reserved bytes.
    std::memset(base, 0, static_cast<std::size_t>(total));

    std::memcpy(header.magic, kUps1Magic, sizeof header.magic);
    header.page_index = source.page_index;
    header.glyph_count = static_cast<std::uint32_t>(glyphs);
    header.term_count =
        static_cast<std::uint32_t>(source.term_hashes.size());
    header.width_pt = source.width_pt;
    header.height_pt = source.height_pt;
    header.crc32 = 0;  // filled below, after the payload is in place
    std::memcpy(base, &header, sizeof header);

    if (glyphs != 0) {
        std::memcpy(base + header.offset_x, source.x.data(),
                    glyphs * sizeof(float));
        std::memcpy(base + header.offset_y, source.y.data(),
                    glyphs * sizeof(float));
        std::memcpy(base + header.offset_w, source.w.data(),
                    glyphs * sizeof(float));
        std::memcpy(base + header.offset_h, source.h.data(),
                    glyphs * sizeof(float));
        std::memcpy(base + header.offset_codepoints,
                    source.codepoints.data(),
                    glyphs * sizeof(std::uint32_t));
    }
    if (!source.term_hashes.empty()) {
        std::memcpy(base + header.offset_term_hashes,
                    source.term_hashes.data(),
                    source.term_hashes.size() * sizeof(std::uint64_t));
    }
    if (!source.postings.empty()) {
        std::memcpy(base + header.offset_postings, source.postings.data(),
                    source.postings.size());
    }

    const std::uint32_t crc =
        slab_crc(base, static_cast<std::size_t>(total));
    std::memcpy(base + kCrcOffset, &crc, sizeof crc);

    return {base, static_cast<std::size_t>(total)};
}

PageSlabView::PageSlabView(std::span<const std::uint8_t> slab)
    : base_(slab.data()), size_(slab.size()) {
    if (base_ == nullptr || size_ < sizeof(PageSlabHeader)) {
        throw PdfToolkitException(ErrorCode::InvalidArgument,
                                  "slab is null or smaller than the 64-byte header");
    }
    if (reinterpret_cast<std::uintptr_t>(base_) % kPageSlabAlignment != 0) {
        throw PdfToolkitException(ErrorCode::InvalidArgument,
                                  "slab base must be 64-byte aligned");
    }
    std::memcpy(&header_, base_, sizeof header_);
    if (std::memcmp(header_.magic, kUps1Magic, sizeof header_.magic) != 0) {
        throw PdfToolkitException(ErrorCode::InvalidArgument,
                                  "slab magic is not UPS1");
    }

    // Structure validation: every section offset is 32-byte aligned,
    // starts after the header, ends inside the slab, and sections appear
    // in canonical order without overlap. (Byte-level integrity is
    // verify_crc()'s job — the ctor pins the STRUCTURE.)
    const auto require = [&](std::uint32_t offset, std::uint64_t bytes,
                             const char* what) -> std::uint64_t {
        if (offset < sizeof(PageSlabHeader)) {
            throw PdfToolkitException(ErrorCode::InvalidArgument,
                                      "slab section offset inside the header");
        }
        if (offset % kSectionAlignment != 0) {
            throw PdfToolkitException(
                ErrorCode::InvalidArgument,
                "slab section offset is not 32-byte aligned");
        }
        const std::uint64_t end = static_cast<std::uint64_t>(offset) + bytes;
        if (end > size_) {
            throw PdfToolkitException(ErrorCode::InvalidArgument, what);
        }
        return end;
    };

    const std::uint64_t glyphs = header_.glyph_count;
    const std::uint64_t end_x =
        require(header_.offset_x, glyphs * sizeof(float), "x array exceeds slab");
    const std::uint64_t end_y = require(header_.offset_y,
                                        glyphs * sizeof(float), "y array exceeds slab");
    const std::uint64_t end_w = require(header_.offset_w,
                                        glyphs * sizeof(float), "w array exceeds slab");
    const std::uint64_t end_h = require(header_.offset_h,
                                        glyphs * sizeof(float), "h array exceeds slab");
    const std::uint64_t end_cp = require(
        header_.offset_codepoints, glyphs * sizeof(std::uint32_t),
        "codepoint array exceeds slab");
    const std::uint64_t end_th = require(
        header_.offset_term_hashes,
        static_cast<std::uint64_t>(header_.term_count) *
            sizeof(std::uint64_t),
        "term-hash array exceeds slab");
    const std::uint64_t end_po = require(header_.offset_postings, 0,
                                         "postings offset exceeds slab");

    if (!(end_x <= header_.offset_y && end_y <= header_.offset_w &&
          end_w <= header_.offset_h && end_h <= header_.offset_codepoints &&
          end_cp <= header_.offset_term_hashes &&
          end_th <= header_.offset_postings && end_po <= size_)) {
        throw PdfToolkitException(ErrorCode::InvalidArgument,
                                  "slab sections overlap or are out of order");
    }

    x_ = {reinterpret_cast<const float*>(base_ + header_.offset_x),
          header_.glyph_count};
    y_ = {reinterpret_cast<const float*>(base_ + header_.offset_y),
          header_.glyph_count};
    w_ = {reinterpret_cast<const float*>(base_ + header_.offset_w),
          header_.glyph_count};
    h_ = {reinterpret_cast<const float*>(base_ + header_.offset_h),
          header_.glyph_count};
    codepoints_ = {
        reinterpret_cast<const std::uint32_t*>(
            base_ + header_.offset_codepoints),
        header_.glyph_count};
    term_hashes_ = {
        reinterpret_cast<const std::uint64_t*>(
            base_ + header_.offset_term_hashes),
        header_.term_count};
    postings_ = {base_ + header_.offset_postings,
                 static_cast<std::size_t>(size_ - header_.offset_postings)};
}

bool PageSlabView::verify_crc() const noexcept {
    if (base_ == nullptr || size_ < sizeof(PageSlabHeader)) {
        return false;
    }
    return slab_crc(base_, size_) == header_.crc32;
}

}  // namespace pdftoolkit::memory
