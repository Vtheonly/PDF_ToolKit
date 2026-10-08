// Backward startxref / trailer scanner tests (audit issue-1/task-2.1).
//
// Acceptance criterion (audit): "Correct offset identification across
// 10,000 PDF test files with variable whitespace and trailing junk
// bytes." The repository has no binary-fixture corpus (tests build
// their own documents — AGENTS.md §4.8), so the acceptance corpus here
// is 10,000 deterministic synthetic documents whose whitespace mix,
// trailer shape, decoy entries and trailing junk all vary per document,
// generated from a seeded LCG. Every document's expected xref offset,
// /Root and /Prev are known by construction and asserted.
//
// The corpus runs twice: with the runtime-dispatched SIMD search and
// with the portable scalar search forced (detail::set_search_force_scalar)
// — the two paths must agree on every document.

#include "test_harness.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "pdftoolkit/errors.hpp"
#include "pdftoolkit/memory/mmap.hpp"
#include "pdftoolkit/parser/trailer.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

using pdftoolkit::ErrorCode;
using pdftoolkit::PdfToolkitException;
using pdftoolkit::parser::TrailerInfo;
using pdftoolkit::parser::locate_startxref;

std::vector<std::uint8_t> operator""_b(const char* literal, std::size_t n) {
    return std::vector<std::uint8_t>(literal, literal + n);
}

void append(std::vector<std::uint8_t>& out, const char* s) {
    out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(s),
               reinterpret_cast<const std::uint8_t*>(s) + std::strlen(s));
}

// --- tiny deterministic LCG -------------------------------------------------

struct Rng {
    std::uint32_t state;
    explicit Rng(std::uint32_t seed) : state(seed * 1664525u + 1013904223u) {}
    std::uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
    std::uint32_t below(std::uint32_t n) { return next() % n; }
};

// 1..3 random PDF whitespace bytes (NUL/TAB/LF/FF/CR/SP).
void add_ws(Rng& rng, std::vector<std::uint8_t>& out) {
    static const std::uint8_t ws[] = {0x00, 0x09, 0x0A, 0x0C, 0x0D, 0x20};
    const std::uint32_t n = 1 + rng.below(3);
    for (std::uint32_t i = 0; i < n; ++i) {
        out.push_back(ws[rng.below(6)]);
    }
}

// --- synthetic corpus document ----------------------------------------------

struct SynthDoc {
    std::vector<std::uint8_t> bytes;
    std::uint64_t expected_offset = 0;
    bool has_root = false;
    std::uint32_t root_object = 0;
    std::uint32_t root_generation = 0;
    bool has_prev = false;
    std::uint64_t prev_offset = 0;
    bool classic_trailer = true;
};

// Random junk bytes. The alphabet deliberately omits 'x' so the junk can
// never accidentally contain the keyword "startxref" (which needs an
// 'x'); the letter 's' is included densely so the SIMD candidate
// rejection paths are exercised on nearly every document. Controlled
// near-miss decoys are injected separately, below.
void add_junk(Rng& rng, std::vector<std::uint8_t>& out, std::uint32_t max_len) {
    static const char alphabet[] =
        "sstt aa rr ee ff bb cc dd gg hh ii jj kk ll mm nn oo pp qq uu vv "
        "ww yy zz 001122334455 !@#$%^&*()_+-=[]{}|;:',.<>/?~";
    constexpr std::uint32_t kAlphabet = sizeof(alphabet) - 1;
    const std::uint32_t n = rng.below(max_len + 1);
    for (std::uint32_t i = 0; i < n; ++i) {
        out.push_back(static_cast<std::uint8_t>(alphabet[rng.below(kAlphabet)]));
    }
}

// A near-miss startxref decoy that must NEVER win: keyword glued to a
// regular character, or keyword + whitespace + non-numeric junk.
void add_invalid_decoy(Rng& rng, std::vector<std::uint8_t>& out) {
    switch (rng.below(4)) {
        case 0: append(out, "startxrefZ"); break;
        case 1: append(out, "startxref\nzz"); break;
        case 2: append(out, "startxre\n77\n"); break;
        default: append(out, "start xref\n77\n"); break;
    }
}

SynthDoc make_doc(std::uint32_t seed) {
    Rng rng(seed);
    SynthDoc doc;

    // Prefix junk (document body stand-in).
    add_junk(rng, doc.bytes, 180);
    append(doc.bytes, "\n");

    // Cross-reference table stand-in; the expected offset points HERE.
    doc.expected_offset = doc.bytes.size();
    append(doc.bytes, "xref\n0 3\n0000000000 65535 f \n");
    append(doc.bytes, "0000000017 00000 n \n0000000060 00000 n \n");
    add_ws(rng, doc.bytes);

    const bool classic = (seed % 11) != 0;  // every 11th: XRef-stream style
    doc.classic_trailer = classic;
    if (classic) {
        append(doc.bytes, "trailer");
        add_ws(rng, doc.bytes);
        append(doc.bytes, "<<");

        // Decoy entries before the real ones.
        if (seed % 5 == 0) {  // nested-dictionary /Root decoy
            append(doc.bytes, "/DecodeParms << /Root 999 7 R >> ");
        }
        if (seed % 6 == 0) {  // literal-string /Root decoy (with brackets!)
            append(doc.bytes, "/Foo (decoy (nested) /Root 555 5 R >> junk) ");
        }
        if (seed % 9 == 0) {  // name-suffix decoy
            append(doc.bytes, "/Root1 42 1 R ");
        }
        if (seed % 10 == 0) {  // hex string + array noise
            append(doc.bytes, "/ID [<A1B2C3D4> <E5F60708>] /Size 9 ");
        }

        // /Root (absent on every 7th classic document).
        doc.has_root = (seed % 7) != 0;
        if (doc.has_root) {
            doc.root_object = rng.below(100000);
            doc.root_generation = rng.below(65536);
            char buf[64];
            if (seed % 4 == 0) {  // zero-padded canonical xref style
                std::snprintf(buf, sizeof(buf), "/Root %010u %05u R ",
                              doc.root_object, doc.root_generation);
            } else {
                std::snprintf(buf, sizeof(buf), "/Root %u %u R ",
                              doc.root_object, doc.root_generation);
            }
            append(doc.bytes, buf);
        }

        // /Prev on every 3rd document.
        doc.has_prev = (seed % 3) == 0;
        if (doc.has_prev) {
            doc.prev_offset = 96 + rng.below(40000);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "/Prev %llu ",
                          static_cast<unsigned long long>(doc.prev_offset));
            append(doc.bytes, buf);
        }

        // /Size keeps the dictionary body non-empty even on documents
        // with no /Root and no decoys (the corpus asserts classic
        // trailers yield a non-empty dict view).
        char size_buf[24];
        std::snprintf(size_buf, sizeof(size_buf), "/Size %u ",
                      3 + rng.below(97));
        append(doc.bytes, size_buf);

        append(doc.bytes, ">>");
        add_ws(rng, doc.bytes);
    } else {
        // XRef-stream style: no classic trailer at all.
        append(doc.bytes, "endobj\n");
    }

    append(doc.bytes, "startxref");
    add_ws(rng, doc.bytes);
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%llu",
                      static_cast<unsigned long long>(doc.expected_offset));
        append(doc.bytes, buf);
    }
    add_ws(rng, doc.bytes);
    append(doc.bytes, "%%EOF");
    if (seed % 3 != 1) {
        append(doc.bytes, "\n");
    }

    // Trailing junk (after %%EOF), sometimes with an invalid decoy.
    add_junk(rng, doc.bytes, 300);
    if (seed % 4 == 0) {
        add_invalid_decoy(rng, doc.bytes);
        add_junk(rng, doc.bytes, 40);
    }
    return doc;
}

void assert_doc(const SynthDoc& doc) {
    const TrailerInfo info =
        locate_startxref(std::span<const std::uint8_t>(doc.bytes));
    PDTK_ASSERT_EQ(info.xref_offset, doc.expected_offset);
    PDTK_ASSERT_EQ(info.has_root, doc.has_root);
    if (doc.has_root) {
        PDTK_ASSERT_EQ(info.root_object, doc.root_object);
        PDTK_ASSERT_EQ(info.root_generation, doc.root_generation);
    }
    PDTK_ASSERT_EQ(info.has_prev, doc.has_prev);
    if (doc.has_prev) {
        PDTK_ASSERT_EQ(info.prev_offset, doc.prev_offset);
    }
    if (doc.classic_trailer) {
        PDTK_ASSERT(!info.dict.empty());
    } else {
        PDTK_ASSERT(info.dict.empty());
    }
}

// ---------------------------------------------------------------------------
// Unit cases
// ---------------------------------------------------------------------------

PDTK_TEST(empty_span_is_invalid_argument) {
    bool threw = false;
    try {
        (void)locate_startxref(std::span<const std::uint8_t>{});
    } catch (const PdfToolkitException& e) {
        threw = true;
        PDTK_ASSERT_EQ(static_cast<int>(e.code()),
                       static_cast<int>(ErrorCode::InvalidArgument));
    }
    PDTK_ASSERT(threw);
}

PDTK_TEST(zero_window_is_invalid_argument) {
    const auto doc = "trailer<<>>startxref\n1\n%%EOF"_b;
    bool threw = false;
    try {
        (void)locate_startxref(std::span<const std::uint8_t>(doc), 0);
    } catch (const PdfToolkitException& e) {
        threw = true;
        PDTK_ASSERT_EQ(static_cast<int>(e.code()),
                       static_cast<int>(ErrorCode::InvalidArgument));
    }
    PDTK_ASSERT(threw);
}

PDTK_TEST(no_startxref_is_unreadable_pdf) {
    const auto doc = "hello world, no pdf structure at all here"_b;
    bool threw = false;
    try {
        (void)locate_startxref(std::span<const std::uint8_t>(doc));
    } catch (const PdfToolkitException& e) {
        threw = true;
        PDTK_ASSERT_EQ(static_cast<int>(e.code()),
                       static_cast<int>(ErrorCode::UnreadablePdf));
    }
    PDTK_ASSERT(threw);
}

PDTK_TEST(minimal_file_finds_offset_and_root) {
    const auto doc =
        "xref\n0 1\n0000000000 65535 f \ntrailer<< /Root 14 2 R >>"
        "startxref\n77\n%%EOF"_b;
    const TrailerInfo info =
        locate_startxref(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT_EQ(info.xref_offset, std::uint64_t{77});
    // keyword_offset really points at the keyword.
    PDTK_ASSERT_EQ(
        std::memcmp(doc.data() + info.keyword_offset, "startxref", 9), 0);
    PDTK_ASSERT(info.has_root);
    PDTK_ASSERT_EQ(info.root_object, std::uint32_t{14});
    PDTK_ASSERT_EQ(info.root_generation, std::uint32_t{2});
    PDTK_ASSERT(!info.has_prev);
}

PDTK_TEST(mixed_whitespace_and_line_endings) {
    // \r\n, bare \r, tabs, formfeed, NUL and runs of spaces everywhere.
    const auto doc =
        "xref\r\n0 1\r\n0000000000 65535 f \r\ntrailer\t\t<< /Root 3 0 R "
        ">>\x0c\x00startxref\r\n \t121\n%%EOF"_b;
    const TrailerInfo info =
        locate_startxref(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT_EQ(info.xref_offset, std::uint64_t{121});
    PDTK_ASSERT(info.has_root);
    PDTK_ASSERT_EQ(info.root_object, std::uint32_t{3});
}

PDTK_TEST(trailing_binary_junk_after_eof) {
    std::vector<std::uint8_t> doc =
        "xref\n0 1\ntrailer<< /Root 5 0 R >>startxref\n42\n%%EOF\n"_b;
    // 's'-dense binary junk, including NUL bytes.
    for (int i = 0; i < 200; ++i) {
        doc.push_back(static_cast<std::uint8_t>((i % 3 == 0) ? 's' : i));
    }
    const TrailerInfo info =
        locate_startxref(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT_EQ(info.xref_offset, std::uint64_t{42});
    PDTK_ASSERT(info.has_root);
}

PDTK_TEST(invalid_decoys_in_junk_are_skipped) {
    auto doc =
        "xref\n0 1\ntrailer<< /Root 5 0 R >>startxref\n4242\n%%EOF\n"_b;
    append(doc, "startxrefZ ... startxref\nnotanumber ... startxre\n9\n");
    const TrailerInfo info =
        locate_startxref(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT_EQ(info.xref_offset, std::uint64_t{4242});
}

PDTK_TEST(last_valid_startxref_wins) {
    // Two valid keywords: an incremental update's startxref follows the
    // original; the backward scan must find the LAST one.
    const auto doc =
        "xref\n0 1\ntrailer<< /Root 1 0 R >>startxref\n111\n%%EOF\n"
        "xref\n0 1\ntrailer<< /Root 2 0 R /Prev 111 >>startxref\n222\n"
        "%%EOF\n"_b;
    const TrailerInfo info =
        locate_startxref(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT_EQ(info.xref_offset, std::uint64_t{222});
    PDTK_ASSERT(info.has_root);
    PDTK_ASSERT_EQ(info.root_object, std::uint32_t{2});
    PDTK_ASSERT(info.has_prev);
    PDTK_ASSERT_EQ(info.prev_offset, std::uint64_t{111});
}

PDTK_TEST(keyword_exactly_at_window_edge_is_found) {
    const std::string head = "trailer<< /Root 1 0 R >>startxref\n7\n%%EOF";
    const std::size_t s_pos = head.find("startxref");
    PDTK_ASSERT(s_pos != std::string::npos);
    std::vector<std::uint8_t> doc(head.begin(), head.end());
    // Pad the tail so the 's' lands exactly at size - 1024 (the last
    // searchable position inside the audit's default window).
    const std::size_t pad = s_pos + 1024 - doc.size();
    doc.insert(doc.end(), pad, static_cast<std::uint8_t>('.'));
    const TrailerInfo info =
        locate_startxref(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT_EQ(info.xref_offset, std::uint64_t{7});
    PDTK_ASSERT_EQ(info.keyword_offset, static_cast<std::uint64_t>(s_pos));
}

PDTK_TEST(keyword_outside_default_window_needs_larger_window) {
    std::vector<std::uint8_t> doc =
        "trailer<< /Root 1 0 R >>startxref\n7\n%%EOF\n"_b;
    doc.insert(doc.end(), 1200, static_cast<std::uint8_t>('.'));
    // Default audit window (1024): honest failure.
    bool threw = false;
    try {
        (void)locate_startxref(std::span<const std::uint8_t>(doc));
    } catch (const PdfToolkitException& e) {
        threw = true;
        PDTK_ASSERT_EQ(static_cast<int>(e.code()),
                       static_cast<int>(ErrorCode::UnreadablePdf));
    }
    PDTK_ASSERT(threw);
    // The window is a parameter: a larger one finds it.
    const TrailerInfo info =
        locate_startxref(std::span<const std::uint8_t>(doc), 4096);
    PDTK_ASSERT_EQ(info.xref_offset, std::uint64_t{7});
}

PDTK_TEST(overflowing_offset_is_rejected) {
    const auto doc =
        "trailer<<>>startxref\n99999999999999999999999999\n%%EOF"_b;
    bool threw = false;
    try {
        (void)locate_startxref(std::span<const std::uint8_t>(doc));
    } catch (const PdfToolkitException& e) {
        threw = true;
        PDTK_ASSERT_EQ(static_cast<int>(e.code()),
                       static_cast<int>(ErrorCode::UnreadablePdf));
    }
    PDTK_ASSERT(threw);
}

PDTK_TEST(offset_requires_digits) {
    const auto doc = "trailer<<>>startxref\nabc\n%%EOF"_b;
    bool threw = false;
    try {
        (void)locate_startxref(std::span<const std::uint8_t>(doc));
    } catch (const PdfToolkitException&) {
        threw = true;
    }
    PDTK_ASSERT(threw);
}

PDTK_TEST(offset_running_into_junk_is_rejected) {
    const auto doc = "trailer<<>>startxref\n12x4\n%%EOF"_b;
    bool threw = false;
    try {
        (void)locate_startxref(std::span<const std::uint8_t>(doc));
    } catch (const PdfToolkitException&) {
        threw = true;
    }
    PDTK_ASSERT(threw);
}

PDTK_TEST(plus_signed_offset_is_accepted) {
    const auto doc = "trailer<<>>startxref\n+321\n%%EOF"_b;
    const TrailerInfo info =
        locate_startxref(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT_EQ(info.xref_offset, std::uint64_t{321});
}

PDTK_TEST(delimiter_terminated_offset_is_accepted) {
    // PDF integer tokens may be terminated by a delimiter: `123%%EOF`
    // is the integer 123 followed by a comment.
    const auto doc = "trailer<<>>startxref\n123%%EOF"_b;
    const TrailerInfo info =
        locate_startxref(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT_EQ(info.xref_offset, std::uint64_t{123});
}

PDTK_TEST(large_64bit_offsets_parse) {
    const auto doc =
        "trailer<<>>startxref\n8589934592\n%%EOF"_b;  // 2^33
    const TrailerInfo a =
        locate_startxref(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT_EQ(a.xref_offset, std::uint64_t{8589934592});

    const auto maxed = "trailer<<>>startxref\n18446744073709551615\n%%EOF"_b;
    const TrailerInfo b =
        locate_startxref(std::span<const std::uint8_t>(maxed));
    PDTK_ASSERT_EQ(b.xref_offset, std::uint64_t{18446744073709551615ULL});
}

PDTK_TEST(empty_trailer_dict_yields_empty_view) {
    // `<<>>` (legal, if unusual): the body view is empty — which is
    // exactly the documented signal for "nothing to extract".
    const auto doc = "trailer<<>>startxref\n3\n%%EOF"_b;
    const TrailerInfo info =
        locate_startxref(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT_EQ(info.xref_offset, std::uint64_t{3});
    PDTK_ASSERT(info.dict.empty());
    PDTK_ASSERT(!info.has_root);
    PDTK_ASSERT(!info.has_prev);
}

PDTK_TEST(root_absent_is_reported_honestly) {
    const auto doc =
        "xref\n0 1\ntrailer<< /Size 4 /ID [<aa> <bb>] >>startxref\n9\n%%EOF"_b;
    const TrailerInfo info =
        locate_startxref(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT_EQ(info.xref_offset, std::uint64_t{9});
    PDTK_ASSERT(!info.has_root);
    PDTK_ASSERT(!info.has_prev);
    PDTK_ASSERT(!info.dict.empty());
}

PDTK_TEST(root_decoy_in_nested_dictionary_is_ignored) {
    const auto doc =
        "trailer<< /DecodeParms << /Root 9 0 R >> /Root 3 0 R >>"
        "startxref\n5\n%%EOF"_b;
    const TrailerInfo info =
        locate_startxref(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT(info.has_root);
    PDTK_ASSERT_EQ(info.root_object, std::uint32_t{3});
}

PDTK_TEST(root_decoy_in_literal_string_is_ignored) {
    const auto doc =
        "trailer<< /Title (not /Root 9 0 R but \\) text) /Root 4 0 R >>"
        "startxref\n5\n%%EOF"_b;
    const TrailerInfo info =
        locate_startxref(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT(info.has_root);
    PDTK_ASSERT_EQ(info.root_object, std::uint32_t{4});
}

PDTK_TEST(root_name_suffix_decoy_is_ignored) {
    const auto doc =
        "trailer<< /Root1 9 0 R /Rooted 8 0 R /Root 6 0 R >>"
        "startxref\n5\n%%EOF"_b;
    const TrailerInfo info =
        locate_startxref(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT(info.has_root);
    PDTK_ASSERT_EQ(info.root_object, std::uint32_t{6});
}

PDTK_TEST(root_reference_must_be_wellformed) {
    // `/Root 3 0` (missing R) is not a reference: absence, not garbage.
    const auto doc =
        "trailer<< /Root 3 0 >>startxref\n5\n%%EOF"_b;
    const TrailerInfo info =
        locate_startxref(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT(!info.has_root);
}

PDTK_TEST(dict_span_is_exactly_the_body_between_brackets) {
    const auto doc =
        "trailer<< /Root 1 0 R /Size 3 >>startxref\n5\n%%EOF"_b;
    const TrailerInfo info =
        locate_startxref(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT(info.dict == std::string_view(" /Root 1 0 R /Size 3 "));
}

PDTK_TEST(strings_and_nesting_do_not_confuse_close_detection) {
    const auto doc =
        "trailer<< /Foo (<< >> (())) /Bar << /Baz <AABBCC> >> /Root 1 0 R >>"
        "startxref\n5\n%%EOF"_b;
    const TrailerInfo info =
        locate_startxref(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT(info.has_root);
    PDTK_ASSERT_EQ(info.root_object, std::uint32_t{1});
    PDTK_ASSERT(info.dict ==
                std::string_view(
                    " /Foo (<< >> (())) /Bar << /Baz <AABBCC> >> /Root 1 0 R "));
}

PDTK_TEST(xref_stream_style_has_no_classic_trailer) {
    // PDF 1.5+ XRef streams carry no `trailer` keyword; their dictionary
    // lives on the stream object (task 2.2 resolves it). Here: offset
    // still found, dictionary honestly empty.
    const auto doc = "12 0 obj\n<</Type/XRef/Root 1 0 R>>\nstream\n...\n"
                     "endstream\nendobj\nstartxref\n456\n%%EOF"_b;
    const TrailerInfo info =
        locate_startxref(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT_EQ(info.xref_offset, std::uint64_t{456});
    PDTK_ASSERT(info.dict.empty());
    PDTK_ASSERT(!info.has_root);
}

PDTK_TEST(keyword_at_buffer_start) {
    const auto doc = "startxref\n5\n%%EOF"_b;
    const TrailerInfo info =
        locate_startxref(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT_EQ(info.xref_offset, std::uint64_t{5});
    PDTK_ASSERT_EQ(info.keyword_offset, std::uint64_t{0});
}

PDTK_TEST(trailer_glued_to_dict_open_is_tolerated) {
    // Sloppy generators emit `trailer<<` with no whitespace.
    const auto doc = "trailer<</Root 2 0 R>>startxref\n18\n%%EOF"_b;
    const TrailerInfo info =
        locate_startxref(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT(info.has_root);
    PDTK_ASSERT_EQ(info.root_object, std::uint32_t{2});
    PDTK_ASSERT_EQ(info.xref_offset, std::uint64_t{18});
}

PDTK_TEST(prev_as_indirect_reference_is_recorded_absent) {
    // Broken files sometimes write /Prev 4 0 R; the scanner must not
    // record 4 as an offset.
    const auto doc =
        "trailer<< /Root 1 0 R /Prev 4 0 R >>startxref\n5\n%%EOF"_b;
    const TrailerInfo info =
        locate_startxref(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT(info.has_root);
    PDTK_ASSERT(!info.has_prev);
}

// ---------------------------------------------------------------------------
// Acceptance: 10,000 synthetic documents, SIMD and scalar paths
// ---------------------------------------------------------------------------

PDTK_TEST(acceptance_ten_thousand_synthetic_documents) {
    constexpr std::uint32_t kCorpus = 10000;
    for (std::uint32_t seed = 0; seed < kCorpus; ++seed) {
        assert_doc(make_doc(seed));
    }
}

PDTK_TEST(scalar_search_path_agrees_with_simd_on_the_corpus) {
    constexpr std::uint32_t kCorpus = 10000;
    pdftoolkit::parser::detail::set_search_force_scalar(true);
    for (std::uint32_t seed = 0; seed < kCorpus; ++seed) {
        assert_doc(make_doc(seed));
    }
    pdftoolkit::parser::detail::set_search_force_scalar(false);
}

#ifndef _WIN32

// --- mmap composition -------------------------------------------------------

class TempPdfFile {
public:
    explicit TempPdfFile(const std::vector<std::uint8_t>& content)
        : path_(std::filesystem::temp_directory_path() /
                ("pdtk_test_trailer_" + std::to_string(::getpid()) +
                 "_XXXXXX")) {
        std::string tmpl = path_.string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        const int fd = ::mkstemp(buf.data());
        PDTK_ASSERT(fd >= 0);
        path_ = buf.data();
        std::size_t written = 0;
        while (written < content.size()) {
            const ssize_t n =
                ::write(fd, content.data() + written, content.size() - written);
            PDTK_ASSERT(n > 0);
            written += static_cast<std::size_t>(n);
        }
        PDTK_ASSERT_EQ(::close(fd), 0);
    }
    ~TempPdfFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempPdfFile(const TempPdfFile&) = delete;
    TempPdfFile& operator=(const TempPdfFile&) = delete;

    [[nodiscard]] const char* c_str() const { return path_.c_str(); }

private:
    std::filesystem::path path_;
};

PDTK_TEST(mmap_backed_scan_is_zero_copy) {
    std::vector<std::uint8_t> content =
        "xref\n0 1\n0000000000 65535 f \ntrailer<< /Root 12 0 R /Prev 33 >>"
        "startxref\n31\n%%EOF\n"_b;
    // Push the tail around with junk so the mmap path scans a real tail.
    Rng rng(7);
    add_junk(rng, content, 400);
    const TempPdfFile file(content);

    const pdftoolkit::memory::MmapHandle map(file.c_str());
    const auto bytes = map.bytes();
    PDTK_ASSERT_EQ(bytes.size(), content.size());

    const TrailerInfo info = locate_startxref(bytes);
    PDTK_ASSERT_EQ(info.xref_offset, std::uint64_t{31});
    PDTK_ASSERT(info.has_root);
    PDTK_ASSERT_EQ(info.root_object, std::uint32_t{12});
    PDTK_ASSERT(info.has_prev);
    PDTK_ASSERT_EQ(info.prev_offset, std::uint64_t{33});

    // Zero-copy proof: every view the scanner returned points INTO the
    // mapping, not at a copy.
    const auto* base = bytes.data();
    const auto* dict_first =
        reinterpret_cast<const std::uint8_t*>(info.dict.data());
    PDTK_ASSERT(dict_first >= base);
    PDTK_ASSERT(dict_first < base + bytes.size());
    PDTK_ASSERT(info.dict.size() <= bytes.size());
}

#endif  // !_WIN32

}  // namespace

PDTK_TEST_MAIN()
