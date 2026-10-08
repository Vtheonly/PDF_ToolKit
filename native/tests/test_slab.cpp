// Unified Page Slab unit tests (audit issue-1/task-1.3).
//
// Acceptance criterion (audit): "Unit tests verifying that coordinate
// offsets are 32-byte aligned for AVX2 instructions" — covered by
// coordinate_offsets_are_32_byte_aligned, which builds slabs with sizes
// deliberately not multiples of 32 (forcing padding) and checks both the
// header offsets and the absolute addresses.

#include "test_harness.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

#include "pdftoolkit/errors.hpp"
#include "pdftoolkit/memory/arena.hpp"
#include "pdftoolkit/memory/page_slab.hpp"

namespace {

using pdftoolkit::ErrorCode;
using pdftoolkit::PdfToolkitException;
using pdftoolkit::memory::BumpArena;
using pdftoolkit::memory::PageSlabHeader;
using pdftoolkit::memory::PageSlabSource;
using pdftoolkit::memory::PageSlabView;
using pdftoolkit::memory::build_page_slab;

// Deterministic glyph data so roundtrips are exact.
struct GlyphData {
    std::uint32_t page_index;
    float width_pt, height_pt;
    std::vector<float> x, y, w, h;
    std::vector<std::uint32_t> codepoints;
    std::vector<std::uint64_t> term_hashes;
    std::vector<std::uint8_t> postings;

    PageSlabSource source() const {
        return PageSlabSource{page_index,
                              width_pt,
                              height_pt,
                              x,
                              y,
                              w,
                              h,
                              codepoints,
                              term_hashes,
                              postings};
    }
};

GlyphData make_glyphs(std::size_t glyph_count, std::size_t term_count,
                      std::size_t postings_bytes, std::uint32_t seed) {
    GlyphData data{7, 612.0F, 792.0F, {}, {}, {}, {}, {}, {}, {}};
    std::uint32_t state = seed;
    const auto next = [&state] {
        state = state * 1664525u + 1013904223u;
        return state;
    };
    data.x.resize(glyph_count);
    data.y.resize(glyph_count);
    data.w.resize(glyph_count);
    data.h.resize(glyph_count);
    data.codepoints.resize(glyph_count);
    for (std::size_t i = 0; i < glyph_count; ++i) {
        data.x[i] = static_cast<float>(next() % 100000) * 0.25F;
        data.y[i] = static_cast<float>(next() % 100000) * 0.5F;
        data.w[i] = static_cast<float>(next() % 1000) * 0.125F;
        data.h[i] = static_cast<float>(next() % 1000) * 0.0625F;
        data.codepoints[i] = 0x1F300u + (next() % 800u);
    }
    data.term_hashes.resize(term_count);
    std::uint64_t hash = 0x123456789ABCDEF0ull + seed;
    for (std::size_t i = 0; i < term_count; ++i) {
        data.term_hashes[i] = hash;
        // wrap-safe increment (~6e9/step: no uint64 wrap before ~3e9
        // terms) keeps the sequence strictly increasing -> sorted
        hash += 0x0000000164557ABFull;
    }
    data.postings.resize(postings_bytes);
    for (std::size_t i = 0; i < postings_bytes; ++i) {
        data.postings[i] = static_cast<std::uint8_t>(next());
    }
    return data;
}

// -----------------------------------------------------------------------
// Layout (compile-time + runtime)
// -----------------------------------------------------------------------

PDTK_TEST(header_is_exactly_one_cache_line) {
    PDTK_ASSERT_EQ(sizeof(PageSlabHeader), 64u);
    PDTK_ASSERT_EQ(alignof(PageSlabHeader), 64u);
    // The header static_asserts in page_slab.hpp already fail the build
    // if this drifts; this runtime check documents the same contract.
}

// -----------------------------------------------------------------------
// Builder + view roundtrip
// -----------------------------------------------------------------------

PDTK_TEST(build_then_view_roundtrips_all_arrays) {
    const GlyphData data = make_glyphs(1000, 64, 256, 42u);
    BumpArena arena(1u << 20);
    const auto slab = build_page_slab(arena, data.source());
    PDTK_ASSERT(!slab.empty());

    const PageSlabView view(slab);
    PDTK_ASSERT_EQ(view.page_index(), data.page_index);
    PDTK_ASSERT_EQ(view.glyph_count(), 1000u);
    PDTK_ASSERT_EQ(view.term_count(), 64u);
    PDTK_ASSERT_EQ(view.header().width_pt, data.width_pt);
    PDTK_ASSERT_EQ(view.header().height_pt, data.height_pt);

    PDTK_ASSERT_EQ(view.x().size(), 1000u);
    PDTK_ASSERT(std::memcmp(view.x().data(), data.x.data(),
                            data.x.size() * sizeof(float)) == 0);
    PDTK_ASSERT(std::memcmp(view.y().data(), data.y.data(),
                            data.y.size() * sizeof(float)) == 0);
    PDTK_ASSERT(std::memcmp(view.w().data(), data.w.data(),
                            data.w.size() * sizeof(float)) == 0);
    PDTK_ASSERT(std::memcmp(view.h().data(), data.h.data(),
                            data.h.size() * sizeof(float)) == 0);
    PDTK_ASSERT(std::memcmp(view.codepoints().data(), data.codepoints.data(),
                            data.codepoints.size() * 4) == 0);
    PDTK_ASSERT(std::memcmp(view.term_hashes().data(),
                            data.term_hashes.data(),
                            data.term_hashes.size() * 8) == 0);
    PDTK_ASSERT(std::memcmp(view.postings().data(), data.postings.data(),
                            data.postings.size()) == 0);
    PDTK_ASSERT(view.verify_crc());
}

PDTK_TEST(empty_page_is_a_valid_slab) {
    const GlyphData data = make_glyphs(0, 0, 0, 1u);
    BumpArena arena(4096);
    const auto slab = build_page_slab(arena, data.source());
    PDTK_ASSERT_EQ(slab.size(), 64u);  // header only

    const PageSlabView view(slab);
    PDTK_ASSERT_EQ(view.glyph_count(), 0u);
    PDTK_ASSERT_EQ(view.term_count(), 0u);
    PDTK_ASSERT(view.x().empty());
    PDTK_ASSERT(view.y().empty());
    PDTK_ASSERT(view.codepoints().empty());
    PDTK_ASSERT(view.term_hashes().empty());
    PDTK_ASSERT(view.postings().empty());
    PDTK_ASSERT(view.verify_crc());
}

PDTK_TEST(two_slabs_coexist_in_one_arena) {
    const GlyphData a = make_glyphs(10, 2, 8, 2u);
    const GlyphData b = make_glyphs(25, 3, 9, 3u);
    BumpArena arena(1u << 16);

    const auto slab_a = build_page_slab(arena, a.source());
    const auto slab_b = build_page_slab(arena, b.source());
    PDTK_ASSERT(!slab_a.empty() && !slab_b.empty());
    PDTK_ASSERT(slab_a.data() != slab_b.data());

    const PageSlabView view_a(slab_a);
    const PageSlabView view_b(slab_b);
    PDTK_ASSERT_EQ(view_a.glyph_count(), 10u);
    PDTK_ASSERT_EQ(view_b.glyph_count(), 25u);
    PDTK_ASSERT_EQ(view_b.x()[7], b.x[7]);  // a's build did not clobber b
    PDTK_ASSERT(view_a.verify_crc());
    PDTK_ASSERT(view_b.verify_crc());
}

// -----------------------------------------------------------------------
// The audit's acceptance criterion: 32-byte alignment
// -----------------------------------------------------------------------

PDTK_TEST(coordinate_offsets_are_32_byte_aligned) {
    // Glyph counts chosen so the raw arrays are NOT multiples of 32
    // bytes (1 glyph = 4 B; 7 glyphs = 28 B): the builder must pad.
    for (const std::size_t glyphs : {std::size_t{0}, std::size_t{1},
                                     std::size_t{7}, std::size_t{33}}) {
        const GlyphData data = make_glyphs(glyphs, 3, 5, 4u);
        BumpArena arena(1u << 16);
        const auto slab = build_page_slab(arena, data.source());
        PDTK_ASSERT(!slab.empty());

        const PageSlabView view(slab);
        const auto& header = view.header();
        for (const std::uint32_t offset :
             {header.offset_x, header.offset_y, header.offset_w,
              header.offset_h, header.offset_codepoints,
              header.offset_term_hashes, header.offset_postings}) {
            PDTK_ASSERT_EQ(offset % 32u, 0u);
        }
        // The audit's criterion verbatim: the *coordinate* arrays are
        // loadable by AVX2 (32-byte) instructions — both relative to the
        // slab start and at the absolute address the CPU will touch.
        PDTK_ASSERT_EQ(reinterpret_cast<std::uintptr_t>(slab.data()) % 64u,
                       0u);  // arena adapter guarantees the slab base
        for (const float* coordinate_array :
             {view.x().data(), view.y().data(), view.w().data(),
              view.h().data()}) {
            PDTK_ASSERT_EQ(
                reinterpret_cast<std::uintptr_t>(coordinate_array) % 32u,
                0u);
        }
        PDTK_ASSERT(view.verify_crc());
    }
}

PDTK_TEST(spans_are_zero_copy_into_the_slab) {
    const GlyphData data = make_glyphs(5, 2, 4, 5u);
    BumpArena arena(4096);
    const auto slab = build_page_slab(arena, data.source());
    const PageSlabView view(slab);
    PDTK_ASSERT(reinterpret_cast<const std::uint8_t*>(view.x().data()) ==
                slab.data() + view.header().offset_x);
    PDTK_ASSERT(reinterpret_cast<const std::uint8_t*>(view.term_hashes().data()) ==
                slab.data() + view.header().offset_term_hashes);
    PDTK_ASSERT(view.postings().data() ==
                slab.data() + view.header().offset_postings);
}

// -----------------------------------------------------------------------
// Builder rejection of malformed sources
// -----------------------------------------------------------------------

PDTK_TEST(builder_rejects_mismatched_array_sizes) {
    GlyphData data = make_glyphs(8, 2, 4, 6u);
    data.y.resize(7);  // one short — everything else stays at 8
    BumpArena arena(4096);
    bool threw = false;
    try {
        const auto slab = build_page_slab(arena, data.source());
        (void)slab;
    } catch (const PdfToolkitException& e) {
        threw = true;
        PDTK_ASSERT_EQ(e.code(), ErrorCode::InvalidArgument);
    }
    PDTK_ASSERT(threw);
}

PDTK_TEST(builder_rejects_unsorted_term_hashes) {
    GlyphData data = make_glyphs(4, 4, 4, 7u);
    std::swap(data.term_hashes[1], data.term_hashes[3]);
    BumpArena arena(4096);
    bool threw = false;
    try {
        const auto slab = build_page_slab(arena, data.source());
        (void)slab;
    } catch (const PdfToolkitException& e) {
        threw = true;
        PDTK_ASSERT_EQ(e.code(), ErrorCode::InvalidArgument);
    }
    PDTK_ASSERT(threw);
}

PDTK_TEST(arena_exhaustion_returns_an_empty_span) {
    const GlyphData data = make_glyphs(64, 4, 32, 8u);  // needs ~1.7 KB
    BumpArena arena(64);                                 // one unit only
    const auto slab = build_page_slab(arena, data.source());
    PDTK_ASSERT(slab.empty());
    PDTK_ASSERT_EQ(arena.used_bytes(), 0u);  // nothing partially written
}

// -----------------------------------------------------------------------
// View rejection of malformed slabs (tampering happens in 64-aligned
// arena copies — the view requires the slab's alignment)
// -----------------------------------------------------------------------

/// 64-aligned mutable copy of a slab, for tampering tests.
struct SlabCopy {
    explicit SlabCopy(std::span<const std::uint8_t> slab)
        : arena(slab.size() + 4096) {
        copy = arena.alloc_slice<std::uint8_t>(slab.size());
        PDTK_ASSERT(!copy.empty() && copy.data() != nullptr);
        // The arena slice is only 1-aligned; force the slab's alignment
        // by allocating through the slab module's own storage unit.
        PDTK_ASSERT(reinterpret_cast<std::uintptr_t>(copy.data()) % 64u ==
                    0u);  // first allocation from a fresh arena is at 0
        std::memcpy(copy.data(), slab.data(), slab.size());
    }
    BumpArena arena;
    std::span<std::uint8_t> copy;
};

PDTK_TEST(view_rejects_bad_magic) {
    const GlyphData data = make_glyphs(4, 2, 4, 9u);
    BumpArena arena(4096);
    const auto slab = build_page_slab(arena, data.source());

    SlabCopy tampered(slab);
    tampered.copy[0] = 'X';
    bool threw = false;
    try {
        const PageSlabView view(tampered.copy);
        (void)view;
    } catch (const PdfToolkitException& e) {
        threw = true;
        PDTK_ASSERT_EQ(e.code(), ErrorCode::InvalidArgument);
    }
    PDTK_ASSERT(threw);
}

PDTK_TEST(view_rejects_misaligned_section_offset) {
    const GlyphData data = make_glyphs(4, 2, 4, 10u);
    BumpArena arena(4096);
    const auto slab = build_page_slab(arena, data.source());

    SlabCopy tampered(slab);
    // offset_x lives at byte 24: set it to 36 (not 32-byte aligned).
    const std::uint32_t bad = 36;
    std::memcpy(tampered.copy.data() + 24, &bad, sizeof bad);
    bool threw = false;
    try {
        const PageSlabView view(tampered.copy);
        (void)view;
    } catch (const PdfToolkitException& e) {
        threw = true;
        PDTK_ASSERT_EQ(e.code(), ErrorCode::InvalidArgument);
    }
    PDTK_ASSERT(threw);
}

PDTK_TEST(view_rejects_out_of_bounds_sections) {
    const GlyphData data = make_glyphs(4, 2, 4, 11u);
    BumpArena arena(4096);
    const auto slab = build_page_slab(arena, data.source());

    SlabCopy tampered(slab);
    const std::uint32_t huge = 0xFFFFFF00u;  // glyph_count: way past the end
    std::memcpy(tampered.copy.data() + 8, &huge, sizeof huge);
    bool threw = false;
    try {
        const PageSlabView view(tampered.copy);
        (void)view;
    } catch (const PdfToolkitException& e) {
        threw = true;
        PDTK_ASSERT_EQ(e.code(), ErrorCode::InvalidArgument);
    }
    PDTK_ASSERT(threw);
}

PDTK_TEST(view_rejects_truncated_and_misaligned_slabs) {
    const GlyphData data = make_glyphs(64, 4, 32, 12u);
    BumpArena arena(1u << 16);
    const auto slab = build_page_slab(arena, data.source());

    // Truncations shorter than the header, or cutting into a fixed-size
    // section (the x array needs 256 bytes from offset 64), are
    // structural errors -> the view constructor throws.
    for (const std::size_t cut : {std::size_t{32}, std::size_t{96}}) {
        bool threw = false;
        try {
            const PageSlabView view(slab.first(cut));
        } catch (const PdfToolkitException& e) {
            threw = true;
            PDTK_ASSERT_EQ(e.code(), ErrorCode::InvalidArgument);
        }
        PDTK_ASSERT(threw);
    }

    // Trimming the TAIL, however, is not a structural error by design:
    // the postings region is delimited by the slab length, so a
    // one-byte-shorter slab still parses — and the CRC is what pins the
    // exact byte length (the ctor validates structure, verify_crc()
    // validates bytes).
    {
        const PageSlabView view(slab.first(slab.size() - 1));
        PDTK_ASSERT_EQ(view.glyph_count(), 64u);
        PDTK_ASSERT(!view.verify_crc());  // the trimmed slab is caught here
    }

    // Null / empty input.
    bool threw = false;
    try {
        const PageSlabView view(std::span<const std::uint8_t>{});
    } catch (const PdfToolkitException& e) {
        threw = true;
        PDTK_ASSERT_EQ(e.code(), ErrorCode::InvalidArgument);
    }
    PDTK_ASSERT(threw);
}

PDTK_TEST(view_rejects_misaligned_slab_base) {
    const GlyphData data = make_glyphs(4, 2, 4, 13u);
    BumpArena arena(4096);
    const auto slab = build_page_slab(arena, data.source());
    bool threw = false;
    try {
        const PageSlabView view(slab.last(slab.size() - 16));  // base+16
    } catch (const PdfToolkitException& e) {
        threw = true;
        PDTK_ASSERT_EQ(e.code(), ErrorCode::InvalidArgument);
    }
    PDTK_ASSERT(threw);
}

// -----------------------------------------------------------------------
// CRC integrity
// -----------------------------------------------------------------------

PDTK_TEST(crc_detects_payload_tampering) {
    const GlyphData data = make_glyphs(16, 4, 16, 14u);
    BumpArena arena(4096);
    const auto slab = build_page_slab(arena, data.source());
    const PageSlabView pristine(slab);
    PDTK_ASSERT(pristine.verify_crc());

    SlabCopy tampered(slab);
    tampered.copy[tampered.copy.size() - 1] ^= 0x01u;  // flip a payload bit
    const PageSlabView view(tampered.copy);
    PDTK_ASSERT_EQ(view.glyph_count(), 16u);  // structure still fine...
    PDTK_ASSERT(!view.verify_crc());          // ...bytes were caught
}

PDTK_TEST(crc32_matches_the_reference_vector) {
    // "123456789" -> 0xCBF43926 (canonical CRC-32 check value).
    const char* check = "123456789";
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(check);
    PDTK_ASSERT_EQ(pdftoolkit::memory::crc32_ieee({bytes, 9}),
                   0xCBF43926u);
    // Empty range -> 0 by definition of the finalized value.
    PDTK_ASSERT_EQ(pdftoolkit::memory::crc32_ieee({}), 0u);
}

}  // namespace

PDTK_TEST_MAIN()
