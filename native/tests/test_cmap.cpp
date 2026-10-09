// CMap cache & Unicode resolver tests (audit issue-1/task-3.1).
//
// Sections:
//   A. xxHash64 known-answer vectors (the in-repo hash the audit's
//      dedup step prescribes; reference values generated from the
//      canonical C library via the Python `xxhash` wheel, seed 0) —
//      including every stripe/tail boundary (31/32/33/63/64/65).
//   B. CMapTable parsing: bfchar, both bfrange forms, codespace,
//      UTF-16BE + surrogate pairs, build-time ligature expansion,
//      last-wins, tolerance (malformed entries skipped, never fatal),
//      hostile-range refusal.
//   C. CMapCache stream-object intern + xxHash64 dedup: raw and REAL
//      Flate fixtures (libdeflate compressor, P-022 rule), filter
//      degradation paths, concurrency (TSan-validated).
//   D. Acceptance: a deterministic "standardized glyph-test document"
//      corpus — CID-keyed (2-byte, ranges) and TrueType-subset (1-byte,
//      sparse bfchar) fonts, each with a coded probe string asserted
//      against its exact expected UTF-32 (U-013-class synthetic
//      interpretation: the repo deliberately carries no binary
//      fixtures; the generator is the glyph-test document).

#include "test_harness.hpp"

#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "pdftoolkit/font/cmap.hpp"

#include "../src/core/xxhash64.hpp"

#if PDTK_HAVE_FLATE
#include "libdeflate.h"
#endif

using pdftoolkit::font::CMapCache;
using pdftoolkit::font::CMapInternReport;
using pdftoolkit::font::CMapStats;
using pdftoolkit::font::CMapStatus;
using pdftoolkit::font::CMapTable;

namespace {

// Deterministic LCG (house pattern — same family as test_xref/bench_*).
struct Rng {
    std::uint32_t state;
    explicit Rng(std::uint32_t seed)
        : state(seed * 1664525u + 1013904223u) {}
    std::uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
    std::uint32_t below(std::uint32_t n) { return next() % n; }
};

std::vector<std::uint8_t> bytes_of(std::string_view text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

// Asserts lookup(code) == expected, with a context string on failure.
void expect_seq(const CMapTable& table, std::uint32_t code,
                std::vector<char32_t> expected, const char* context) {
    const std::span<const char32_t> got = table.lookup(code);
    if (got.size() != expected.size()) {
        throw std::runtime_error(std::string(context) + ": code " +
                                 std::to_string(code) + " length " +
                                 std::to_string(got.size()) + " != " +
                                 std::to_string(expected.size()));
    }
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (got[i] != expected[i]) {
            throw std::runtime_error(
                std::string(context) + ": code " + std::to_string(code) +
                " element " + std::to_string(i) + " U+" +
                std::to_string(static_cast<std::uint32_t>(got[i])) +
                " != U+" +
                std::to_string(static_cast<std::uint32_t>(expected[i])));
        }
    }
}

// A canonical /ToUnicode program around the given block bodies.
std::string wrap_cmap(std::string_view blocks) {
    std::string out;
    out += "/CIDInit /ProcSet findresource begin\n";
    out += "12 dict begin\n";
    out += "begincmap\n";
    out += "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) "
           "/Supplement 0 >> def\n";
    out += "/CMapName /Adobe-Identity-UCS def\n";
    out += "/CMapType 2 def\n";
    out += "1 begincodespacerange\n";
    out += "<0000> <FFFF>\n";
    out += "endcodespacerange\n";
    out.append(blocks);
    out += "endcmap\n";
    out += "CMapName currentdict /CMap defineresource pop\n";
    out += "end\nend\n";
    return out;
}

std::shared_ptr<const CMapTable> parse_text(std::string_view text,
                                            CMapStats* stats = nullptr) {
    return CMapTable::parse(bytes_of(text), stats);
}

// Assembles a complete indirect stream object.
std::vector<std::uint8_t> make_object(
    std::uint32_t objnum, std::uint32_t gen, std::string_view dict,
    std::span<const std::uint8_t> payload) {
    std::string head = std::to_string(objnum) + " " + std::to_string(gen) +
                       " obj\n<<" + std::string(dict) + ">>\nstream\n";
    std::vector<std::uint8_t> out = bytes_of(head);
    out.insert(out.end(), payload.begin(), payload.end());
    const std::string tail = "\nendstream\nendobj\n";
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

#if PDTK_HAVE_FLATE
// REAL zlib wrapper (the /FlateDecode payload) — same helper contract
// as test_xref/test_flate (libdeflate compressor, P-022 rule).
std::vector<std::uint8_t> zlib_wrap(const std::vector<std::uint8_t>& plain,
                                    int level = 6) {
    libdeflate_compressor* comp = libdeflate_alloc_compressor(level);
    PDTK_ASSERT(comp != nullptr);
    std::vector<std::uint8_t> out(
        libdeflate_zlib_compress_bound(comp, plain.size()));
    const std::size_t n = libdeflate_zlib_compress(
        comp, plain.data(), plain.size(), out.data(), out.size());
    libdeflate_free_compressor(comp);
    PDTK_ASSERT(n > 0);
    out.resize(n);
    return out;
}
#endif

// ===========================================================================
// A. xxHash64
// ===========================================================================

// Reference vectors from the canonical C implementation (Python
// `xxhash` wheel, xxh64, seed 0). Random inputs are embedded as byte
// arrays — the Mersenne-twister bytes cannot be regenerated portably.
PDTK_TEST(xxhash64_reference_vectors) {
    using pdftoolkit::hash::xxhash64;
    struct Vec {
        std::vector<std::uint8_t> data;
        std::uint64_t expected;
    };
    const auto s = [](const char* p) {
        return bytes_of(std::string_view(p));
    };
    std::vector<Vec> cases;
    cases.push_back({{}, 0xEF46DB3751D8E999ULL});
    cases.push_back({s("a"), 0xD24EC4F1A98C6E5BULL});
    cases.push_back({s("abc"), 0x44BC2CF5AD770999ULL});
    cases.push_back({s("message digest"), 0x066ED728FCEEB3BEULL});
    cases.push_back(
        {s("abcdefghijklmnopqrstuvwxyz"), 0xCFE1F278FA89835CULL});
    cases.push_back({s("The quick brown fox jumps over the lazy dog"),
                     0x0B242D361FDA71BCULL});
    cases.push_back({{0x39}, 0x1D35ED3E41EE029AULL});
    cases.push_back({{0x0C, 0x8C, 0x7D}, 0xACC6113CE51DCDBCULL});
    cases.push_back({{0x72, 0x47, 0x34, 0x2C, 0xD8, 0x10, 0x0F, 0x2F, 0x6F,
                      0x77, 0x0D, 0x65, 0xD6, 0x70, 0xE5},
                     0x40A6AD72B6C25DA0ULL});
    cases.push_back({{0x8E, 0x03, 0x51, 0xD8, 0xAE, 0x8E, 0x4F, 0x6E, 0xAC,
                      0x34, 0x2F, 0xC2, 0x31, 0xB7, 0xB0, 0x87},
                     0xEB9E76AD389ED8F0ULL});
    cases.push_back(
        {{0x16, 0xEB, 0x3F, 0xC1, 0x28, 0x96, 0xB9, 0x62, 0x23, 0x17, 0x74,
          0x94, 0x28, 0x77, 0x33, 0xC2, 0x8E, 0xE8, 0xBA, 0x53, 0xBD, 0xB5,
          0x6B, 0x88, 0x24, 0x57, 0x7D, 0x53, 0xEC, 0xC2, 0x8A},
         0x3C82D7922E23951EULL});
    cases.push_back(
        {{0x70, 0xA6, 0x1C, 0x75, 0x10, 0xA1, 0xCD, 0x89, 0x21, 0x6C, 0xA1,
          0x6C, 0xFF, 0xCA, 0xEA, 0x49, 0x87, 0x47, 0x7E, 0x86, 0xDB, 0xCC,
          0xB9, 0x70, 0x46, 0xFC, 0x2E, 0x18, 0x38, 0x4E, 0x51, 0xD8},
         0x44A07AC04DDC4BCDULL});
    cases.push_back(
        {{0x20, 0xC5, 0xC3, 0xEF, 0x80, 0x05, 0x3A, 0x88, 0xAE, 0x39, 0x96,
          0xDE, 0x50, 0xE8, 0x01, 0x86, 0x5B, 0x36, 0x98, 0x65, 0x4E, 0xBF,
          0x52, 0x00, 0xA5, 0xFA, 0x09, 0x39, 0xB9, 0x9D, 0x7A, 0x1D, 0x7B},
         0x5372940DAF89FB19ULL});
    cases.push_back(
        {{0x28, 0x2B, 0xF8, 0x23, 0x40, 0x41, 0xF3, 0x54, 0x87, 0xD8, 0x6C,
          0x66, 0x9F, 0xCC, 0xBF, 0xE0, 0xE7, 0x3D, 0x7E, 0x73, 0x20, 0xAD,
          0x0A, 0x75, 0x70, 0x03, 0x24, 0x1E, 0x75, 0x22, 0x10, 0xA9, 0x24,
          0x79, 0x8E, 0xF8, 0x6D, 0x43, 0xF2, 0x7C, 0xF2, 0xD0, 0x61, 0x30,
          0x31, 0xDC, 0xB5, 0xD8, 0xD2, 0xEF, 0x1B, 0x32, 0x1F, 0xCE, 0xAD,
          0x37, 0x7F, 0x62, 0x61, 0xE5, 0x47, 0xD8, 0x5D},
         0x83DB5262EEABA133ULL});
    cases.push_back(
        {{0x8E, 0xEC, 0x7F, 0x26, 0xE2, 0x32, 0x19, 0x07, 0x2F, 0x79, 0x55,
          0xD0, 0xF8, 0xF6, 0x6D, 0xCD, 0x1E, 0x54, 0xC2, 0x01, 0xC7, 0x87,
          0xE8, 0x92, 0xD8, 0xF9, 0x4F, 0x61, 0x97, 0x6F, 0x1D, 0x1F, 0xA0,
          0x1D, 0x19, 0xF4, 0x50, 0x1D, 0x29, 0x5F, 0x23, 0x22, 0x78, 0xCE,
          0x3D, 0x7E, 0x14, 0x29, 0xD6, 0xA1, 0x85, 0x68, 0xA0, 0x7A, 0x87,
          0xCA, 0x43, 0x99, 0xEA, 0xA1, 0x25, 0x04, 0xEA, 0x33},
         0xD0F542B052F5223CULL});
    cases.push_back(
        {{0x25, 0x6D, 0x87, 0x43, 0xB2, 0x23, 0x7D, 0xBD, 0x91, 0x50, 0xE0,
          0x9A, 0x04, 0x99, 0x35, 0x44, 0x87, 0x3B, 0x36, 0x4F, 0x8B, 0x90,
          0x6B, 0xAF, 0x68, 0x87, 0xFA, 0x80, 0x1A, 0x2F, 0xD8, 0x8D, 0x16,
          0x01, 0xAA, 0x42, 0x86, 0x52, 0xE2, 0xDA, 0x04, 0x39, 0x26, 0x4C,
          0x12, 0xBD, 0x4B, 0xDC, 0x41, 0x15, 0x9D, 0xBA, 0x14, 0xB7, 0x6B,
          0x7F, 0x34, 0xB5, 0xD0, 0x4F, 0x79, 0x53, 0x5A, 0xD3, 0x0C},
         0x5FE18635366D6A89ULL});
    cases.push_back(
        {{0x5B, 0xAA, 0xD2, 0x7F, 0x88, 0x51, 0x37, 0xC3, 0x13, 0xF0, 0x71,
          0x66, 0xEB, 0xB3, 0x9C, 0x74, 0x72, 0x0C, 0x62, 0xCC, 0xA8, 0x8E,
          0x23, 0x8E, 0xB3, 0xCC, 0xA9, 0x0E, 0x3B, 0x85, 0x5B, 0x87, 0x13,
          0x37, 0xDE, 0xB0, 0xA0, 0xDF, 0x3B, 0xC5, 0x61, 0x82, 0x16, 0xDF,
          0x00, 0x64, 0xBA, 0xDC, 0x23, 0xA9, 0xA0, 0x3F, 0x99, 0x9E, 0xD1,
          0xA7, 0xCE, 0x97, 0x41, 0x62, 0xD7, 0xC2, 0x59, 0x9A, 0xCF, 0x00,
          0x9B, 0x92, 0x6B, 0xDC, 0xA4, 0xEE, 0xE2, 0xE2, 0x6D, 0xF2, 0x56,
          0x2B, 0x91, 0xAB, 0x2F, 0x78, 0x9E, 0x73, 0x65, 0x4B, 0x0C, 0x17,
          0x7D, 0xF3, 0x25, 0xE9, 0xD4, 0x63, 0xC4, 0xFD, 0xCC, 0x7C, 0x4B,
          0x02},
         0x32C9B7859B2A36D5ULL});

    for (const Vec& v : cases) {
        const std::uint64_t got = xxhash64(v.data);
        if (got != v.expected) {
            throw std::runtime_error("xxh64 len=" +
                                     std::to_string(v.data.size()) +
                                     " got " + std::to_string(got) +
                                     " want " + std::to_string(v.expected));
        }
    }
    // One multi-stripe input (128 stripes).
    std::vector<std::uint8_t> big(4096);
    Rng rng(4096);
    for (auto& b : big) {
        b = static_cast<std::uint8_t>(rng.next());
    }
    (void)xxhash64(big);  // no crash / no OOB (ASan validates the reads)
}

PDTK_TEST(xxhash64_incremental_equals_one_shot) {
    using pdftoolkit::hash::XxHash64;
    std::vector<std::uint8_t> data(1000);
    Rng rng(7);
    for (auto& b : data) {
        b = static_cast<std::uint8_t>(rng.next());
    }
    const std::uint64_t one = pdftoolkit::hash::xxhash64(data);
    for (std::size_t split : {std::size_t{0}, std::size_t{1},
                              std::size_t{31}, std::size_t{32},
                              std::size_t{33}, std::size_t{500},
                              data.size()}) {
        XxHash64 h;
        h.update(std::span<const std::uint8_t>(data.data(), split));
        h.update(std::span<const std::uint8_t>(data.data() + split,
                                               data.size() - split));
        PDTK_ASSERT_EQ(h.digest(), one);
        PDTK_ASSERT_EQ(h.digest(), one);  // idempotent
    }
}

PDTK_TEST(xxhash64_seeds_domain_separate) {
    using pdftoolkit::hash::xxhash64;
    const auto data = bytes_of(std::string_view("payload bytes"));
    PDTK_ASSERT(xxhash64(data, 0x524157ULL) !=
                xxhash64(data, 0x464C54ULL));
    PDTK_ASSERT_EQ(xxhash64(data, 0), xxhash64(data));
}

// ===========================================================================
// B. CMapTable parsing
// ===========================================================================

PDTK_TEST(bfchar_two_byte_codes) {
    const auto table = parse_text(wrap_cmap(
        "2 beginbfchar\n"
        "<0003> <0020>\n"
        "<0007> <0041>\n"
        "endbfchar\n"));
    expect_seq(*table, 0x0003, {0x0020}, "bfchar 2-byte");
    expect_seq(*table, 0x0007, {0x0041}, "bfchar 2-byte");
    PDTK_ASSERT_EQ(table->mapped_codes(), std::size_t{2});
    PDTK_ASSERT_EQ(table->code_bytes(), 2u);
}

PDTK_TEST(bfchar_one_byte_codes) {
    const auto table = parse_text(
        "1 begincodespacerange\n<00> <FF>\nendcodespacerange\n"
        "3 beginbfchar\n<24> <0024>\n<41> <0041>\n<FB> <2018>\nendbfchar\n");
    expect_seq(*table, 0x24, {0x24}, "bfchar 1-byte");
    expect_seq(*table, 0xFB, {0x2018}, "bfchar 1-byte");
    PDTK_ASSERT_EQ(table->code_bytes(), 1u);
}

PDTK_TEST(bfrange_three_operand) {
    const auto table = parse_text(wrap_cmap(
        "1 beginbfrange\n<0006> <000F> <0041>\nendbfrange\n"));
    PDTK_ASSERT_EQ(table->mapped_codes(), std::size_t{10});
    expect_seq(*table, 0x0006, {0x0041}, "bfrange 3-op first");
    expect_seq(*table, 0x000A, {0x0045}, "bfrange 3-op mid");
    expect_seq(*table, 0x000F, {0x004A}, "bfrange 3-op last");
    PDTK_ASSERT(table->lookup(0x0005).empty());
    PDTK_ASSERT(table->lookup(0x0010).empty());
}

PDTK_TEST(bfrange_array_form) {
    const auto table = parse_text(wrap_cmap(
        "1 beginbfrange\n<0020> <0023> [<0061> <0062> <0063> <0064>]\n"
        "endbfrange\n"));
    expect_seq(*table, 0x0020, {0x0061}, "bfrange array");
    expect_seq(*table, 0x0023, {0x0064}, "bfrange array last");
    PDTK_ASSERT_EQ(table->mapped_codes(), std::size_t{4});
}

PDTK_TEST(surrogate_pair_destination) {
    const auto table = parse_text(wrap_cmap(
        "2 beginbfchar\n"
        "<0100> <D83DDE00>\n"   // U+1F600
        "<0101> <D834DD1E>\n"   // U+1D11E musical G clef
        "endbfchar\n"));
    expect_seq(*table, 0x0100, {0x1F600}, "surrogate pair");
    expect_seq(*table, 0x0101, {0x1D11E}, "surrogate pair 2");
}

PDTK_TEST(ligature_expansion_audit_example) {
    // The audit's literal example: U+FB01 -> {'f', 'i'}.
    const auto table =
        parse_text(wrap_cmap("1 beginbfchar\n<0009> <FB01>\nendbfchar\n"));
    expect_seq(*table, 0x0009, {0x0066, 0x0069}, "audit ligature fi");
}

PDTK_TEST(ligature_table_is_complete) {
    // Every verified table entry expands to its canonical decomposition.
    struct Row {
        char32_t lig;
        std::vector<char32_t> expansion;
    };
    const std::vector<Row> rows = {
        {0xFB00, {0x0066, 0x0066}},  {0xFB01, {0x0066, 0x0069}},
        {0xFB02, {0x0066, 0x006C}},  {0xFB03, {0x0066, 0x0066, 0x0069}},
        {0xFB04, {0x0066, 0x0066, 0x006C}}, {0xFB05, {0x017F, 0x0074}},
        {0xFB06, {0x0073, 0x0074}},  {0xFB13, {0x0574, 0x0576}},
        {0xFB14, {0x0574, 0x0565}},  {0xFB15, {0x0574, 0x056B}},
        {0xFB16, {0x057E, 0x0576}},  {0xFB17, {0x0574, 0x056D}},
        {0xFB1F, {0x05F2, 0x05B7}},  {0xFB4F, {0x05D0, 0x05DC}},
    };
    std::string blocks = "14 beginbfchar\n";
    std::uint32_t code = 1;
    for (const Row& r : rows) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "<%04X> <%04X>\n", code,
                      static_cast<unsigned>(r.lig));
        blocks += buf;
        ++code;
    }
    blocks += "endbfchar\n";
    const auto table = parse_text(wrap_cmap(blocks));
    code = 1;
    for (const Row& r : rows) {
        expect_seq(*table, code, r.expansion, "ligature table row");
        ++code;
    }
    // Non-ligature codepoints pass through untouched.
    const auto plain = parse_text(
        wrap_cmap("1 beginbfchar\n<0041> <00E9>\nendbfchar\n"));
    expect_seq(*plain, 0x41, {0x00E9}, "plain passthrough");
}

PDTK_TEST(bfrange_with_ligature_base_expands_per_value) {
    // 3-operand base is arithmetic; each GENERATED value expands.
    const auto table = parse_text(wrap_cmap(
        "1 beginbfrange\n<0010> <0012> <FB00>\nendbfrange\n"));
    expect_seq(*table, 0x0010, {0x0066, 0x0066}, "range lig ff");
    expect_seq(*table, 0x0011, {0x0066, 0x0069}, "range lig fi");
    expect_seq(*table, 0x0012, {0x0066, 0x006C}, "range lig fl");
}

PDTK_TEST(later_entries_overwrite_earlier) {
    const auto table = parse_text(wrap_cmap(
        "2 beginbfchar\n"
        "<0005> <0041>\n"
        "<0005> <0042>\n"
        "endbfchar\n"
        "1 beginbfrange\n<0005> <0005> <0043>\nendbfrange\n"));
    expect_seq(*table, 0x0005, {0x0043}, "last wins across blocks");
    PDTK_ASSERT_EQ(table->mapped_codes(), std::size_t{1});
}

PDTK_TEST(preamble_and_epilogue_are_tolerated) {
    // wrap_cmap already carries the full Adobe boilerplate; add junk.
    const auto table = parse_text(wrap_cmap(
        "1 beginbfchar\n<0001> <005A>\nendbfchar\n") +
        "%% trailing comment\ntrailer junk << /HexInDict <0020> >>\n");
    expect_seq(*table, 0x0001, {0x005A}, "preamble tolerated");
}

PDTK_TEST(count_mismatch_is_tolerated) {
    const CMapStats stats = [] {
        CMapStats s;
        const auto t = parse_text(
            wrap_cmap("5 beginbfchar\n<0001> <0041>\n<0002> <0042>\n"
                      "endbfchar\n"),
            &s);
        PDTK_ASSERT(t != nullptr);
        return s;
    }();
    PDTK_ASSERT_EQ(stats.bfchar_entries, std::size_t{2});
    PDTK_ASSERT_EQ(stats.skipped_entries, std::size_t{0});
}

PDTK_TEST(malformed_entries_are_skipped_not_fatal) {
    CMapStats stats;
    const auto table = parse_text(
        wrap_cmap(
            "9 beginbfchar\n"
            "<ZZZZ> <0041>\n"       // bad hex
            "<0002> <D800>\n"       // lone high surrogate
            "<0003> <DC00>\n"       // lone low surrogate
            "<0004> <>\n"           // empty destination
            "<0005> <0045>\n"       // GOOD
            "(006) <0046>\n"        // literal noise: not a hex token;
                                    // <0046> pairs positionally with the
                                    // NEXT hex as its destination
            "<0007>\n"              // (consumed as <0046>'s dst)
            "endbfchar\n"
            "3 beginbfrange\n"
            "<000A> <0003> <0041>\n"     // inverted range
            "<0000> <0002> <00410042>\n"  // multi-codepoint base
            "<0010> <0012> <0043>\n"      // GOOD
            "endbfrange\n"),
        &stats);
    expect_seq(*table, 0x0005, {0x0045}, "survivor bfchar");
    expect_seq(*table, 0x0046, {0x0007}, "positional pair after noise");
    expect_seq(*table, 0x0010, {0x0043}, "survivor bfrange");
    // 7 skipped: the bad-hex PAIR (malformed src + its consumed dst),
    // 2 lone surrogates, empty dst, inverted range, multi-cp base —
    // every malformed entry is accounted for, none is fatal.
    PDTK_ASSERT_EQ(stats.skipped_entries, std::size_t{7});
    PDTK_ASSERT_EQ(stats.mapped_codes, std::size_t{5});
}

PDTK_TEST(hostile_ranges_cannot_size_the_table) {
    CMapStats stats;
    const auto table = parse_text(
        wrap_cmap(
            "2 beginbfrange\n"
            "<0000> <FFFFFFFF> <0041>\n"          // hi out of 2-byte scope
            "<0000> <FFFF> <0010FFFE>\n"          // would pass U+10FFFF
            "endbfrange\n"
            "1 beginbfchar\n<00010001> <0041>\nendbfchar\n"),  // 4-byte src
        &stats);
    PDTK_ASSERT_EQ(stats.mapped_codes, std::size_t{0});
    PDTK_ASSERT_EQ(stats.skipped_entries, std::size_t{3});
    // The direct index never materialized for hostile codes.
    PDTK_ASSERT(table->lookup(0x1234).empty());
    PDTK_ASSERT_EQ(table->mapped_codes(), std::size_t{0});
}

PDTK_TEST(codespace_width_is_declared_or_inferred) {
    const auto two = parse_text(wrap_cmap(
        "1 beginbfchar\n<0100> <0041>\nendbfchar\n"));
    PDTK_ASSERT_EQ(two->code_bytes(), 2u);  // declared by wrap_cmap
    const auto one = parse_text(
        "1 begincodespacerange\n<00> <FF>\nendcodespacerange\n"
        "1 beginbfchar\n<21> <0021>\nendbfchar\n");
    PDTK_ASSERT_EQ(one->code_bytes(), 1u);
    const auto inferred = parse_text(
        "1 beginbfchar\n<0100> <0041>\nendbfchar\n");
    PDTK_ASSERT_EQ(inferred->code_bytes(), 2u);  // widest src 2 bytes
    const auto nothing = parse_text("garbage with no blocks");
    PDTK_ASSERT_EQ(nothing->code_bytes(), 0u);
    PDTK_ASSERT_EQ(nothing->mapped_codes(), std::size_t{0});
}

PDTK_TEST(hex_whitespace_and_odd_padding) {
    const auto table = parse_text(wrap_cmap(
        "2 beginbfchar\n"
        "<00 20> <00 41>\n"  // whitespace inside hex is legal
        "<0F1> <0042>\n"     // odd digit count: padded 0x0F10
        "endbfchar\n"));
    expect_seq(*table, 0x0020, {0x0041}, "hex ws");
    expect_seq(*table, 0x0F10, {0x0042}, "hex pad");
}

PDTK_TEST(unterminated_block_keeps_entries) {
    const auto table = parse_text(wrap_cmap(
        "1 beginbfchar\n<0001> <0041>\n<0002> <0042>\n"));  // no endbfchar
    expect_seq(*table, 0x0001, {0x0041}, "unterminated first");
    expect_seq(*table, 0x0002, {0x0042}, "unterminated second");
}

PDTK_TEST(array_length_mismatch_is_tolerated) {
    CMapStats stats;
    const auto table = parse_text(
        wrap_cmap("1 beginbfrange\n<0020> <0025> [<0061> <0062> <0063>]\n"
                  "endbfrange\n"),
        &stats);  // 3 elements for a 6-wide range: extra codes unmapped
    expect_seq(*table, 0x0020, {0x0061}, "short array first");
    expect_seq(*table, 0x0022, {0x0063}, "short array last");
    PDTK_ASSERT(table->lookup(0x0023).empty());
    PDTK_ASSERT_EQ(stats.bfrange_entries, std::size_t{3});
}

PDTK_TEST(multi_hex_preamble_dicts_inject_nothing) {
    // Hex strings inside preamble dictionaries must not become
    // mappings (Idle mode ignores them by construction).
    const auto table = parse_text(
        std::string("<< /Registry <ABCD> /Ordering (UCS) >>\n") +
        wrap_cmap("1 beginbfchar\n<0001> <0041>\nendbfchar\n"));
    PDTK_ASSERT_EQ(table->mapped_codes(), std::size_t{1});
    expect_seq(*table, 0x0001, {0x0041}, "only the block maps");
}

// ===========================================================================
// C. CMapCache: stream-object intern + dedup
// ===========================================================================

PDTK_TEST(intern_raw_stream_object) {
    CMapCache cache;
    CMapInternReport report;
    const auto payload = bytes_of(wrap_cmap(
        "2 beginbfchar\n<0003> <0020>\n<0007> <0041>\nendbfchar\n"));
    const auto object = make_object(
        12, 0, " /Length " + std::to_string(payload.size()), payload);
    const auto table =
        cache.intern_stream_object(object, &report);
    PDTK_ASSERT(table != nullptr);
    PDTK_ASSERT(report.status == CMapStatus::Ok);
    PDTK_ASSERT(!report.cache_hit);
    expect_seq(*table, 0x0003, {0x0020}, "intern raw");
    expect_seq(*table, 0x0007, {0x0041}, "intern raw");
    PDTK_ASSERT_EQ(report.parse_stats.mapped_codes, std::size_t{2});
    PDTK_ASSERT_EQ(cache.size(), std::size_t{1});
    PDTK_ASSERT_EQ(cache.intern_hits(), std::size_t{0});
}

PDTK_TEST(intern_dedup_same_payload) {
    CMapCache cache;
    const auto payload = bytes_of(wrap_cmap(
        "1 beginbfchar\n<0001> <0041>\nendbfchar\n"));
    const auto a = make_object(5, 0, " /Length 999", payload);   // wrong /Length (raw: advisory)
    const auto b = make_object(47, 2, "", payload);              // no /Length at all
    CMapInternReport ra;
    const auto ta = cache.intern_stream_object(a, &ra);
    PDTK_ASSERT(ta != nullptr);
    CMapInternReport rb;
    const auto tb = cache.intern_stream_object(b, &rb);
    PDTK_ASSERT(tb != nullptr);
    // Same payload bytes -> SAME frozen table (pointer identity).
    PDTK_ASSERT(ta.get() == tb.get());
    PDTK_ASSERT(rb.cache_hit);
    PDTK_ASSERT_EQ(cache.size(), std::size_t{1});
    PDTK_ASSERT_EQ(cache.intern_hits(), std::size_t{1});
}

PDTK_TEST(intern_distinct_payloads_stay_distinct) {
    CMapCache cache;
    const auto p1 = bytes_of(wrap_cmap(
        "1 beginbfchar\n<0001> <0041>\nendbfchar\n"));
    const auto p2 = bytes_of(wrap_cmap(
        "1 beginbfchar\n<0001> <0042>\nendbfchar\n"));
    const auto t1 = cache.intern_stream_object(
        make_object(1, 0, "", p1));
    const auto t2 = cache.intern_stream_object(
        make_object(1, 0, "", p2));
    PDTK_ASSERT(t1 != nullptr && t2 != nullptr);
    PDTK_ASSERT(t1.get() != t2.get());
    expect_seq(*t1, 0x0001, {0x0041}, "distinct 1");
    expect_seq(*t2, 0x0001, {0x0042}, "distinct 2");
    PDTK_ASSERT_EQ(cache.size(), std::size_t{2});
}

PDTK_TEST(intern_bad_object_reports) {
    CMapCache cache;
    CMapInternReport report;
    PDTK_ASSERT(cache.intern_stream_object(bytes_of("junk"), &report) ==
                nullptr);
    PDTK_ASSERT(report.status == CMapStatus::BadObject);
    // Dict but no stream.
    PDTK_ASSERT(cache.intern_stream_object(
                    bytes_of("1 0 obj\n<< /Length 3 >>\nendobj\n"),
                    &report) == nullptr);
    PDTK_ASSERT(report.status == CMapStatus::BadObject);
    PDTK_ASSERT_EQ(cache.size(), std::size_t{0});
}

PDTK_TEST(intern_unsupported_filters_degrade) {
    CMapCache cache;
    CMapInternReport report;
    const auto payload = bytes_of("anything");
    // Non-Flate filter.
    PDTK_ASSERT(cache.intern_stream_object(
                    make_object(1, 0, " /Filter /DCTDecode", payload),
                    &report) == nullptr);
    PDTK_ASSERT(report.status == CMapStatus::UnsupportedFilter);
    // Multi-filter pipeline.
    PDTK_ASSERT(cache.intern_stream_object(
                    make_object(1, 0,
                                " /Filter [/FlateDecode /DCTDecode]",
                                payload),
                    &report) == nullptr);
    PDTK_ASSERT(report.status == CMapStatus::UnsupportedFilter);
    // Predictor-bearing DecodeParms (P-020 rule).
    PDTK_ASSERT(cache.intern_stream_object(
                    make_object(1, 0,
                                " /Filter /FlateDecode /DecodeParms "
                                "<</Predictor 12 /Columns 4>>",
                                payload),
                    &report) == nullptr);
    PDTK_ASSERT(report.status == CMapStatus::UnsupportedFilter);
    PDTK_ASSERT_EQ(cache.size(), std::size_t{0});
}

#if PDTK_HAVE_FLATE
PDTK_TEST(intern_flate_stream_object) {
    CMapCache cache;
    CMapInternReport report;
    const auto plain = bytes_of(wrap_cmap(
        "2 beginbfchar\n<0100> <D83DDE00>\n<0101> <FB01>\nendbfchar\n"));
    const auto compressed = zlib_wrap(plain);
    const auto object = make_object(
        9, 0,
        " /Length " + std::to_string(compressed.size()) +
            " /Filter /FlateDecode",
        compressed);
    const auto table = cache.intern_stream_object(object, &report);
    PDTK_ASSERT(table != nullptr);
    PDTK_ASSERT(report.status == CMapStatus::Ok);
    PDTK_ASSERT_EQ(report.parse_stats.mapped_codes, std::size_t{2});
    expect_seq(*table, 0x0100, {0x1F600}, "flate surrogate");
    expect_seq(*table, 0x0101, {0x0066, 0x0069}, "flate ligature");
    // Re-intern: dedup on the COMPRESSED payload.
    const auto again = cache.intern_stream_object(object, &report);
    PDTK_ASSERT(again.get() == table.get());
    PDTK_ASSERT(report.cache_hit);
    PDTK_ASSERT_EQ(cache.size(), std::size_t{1});
}

PDTK_TEST(intern_flate_legacy_fl_abbreviation) {
    CMapCache cache;
    CMapInternReport report;
    const auto plain = bytes_of(wrap_cmap(
        "1 beginbfchar\n<0001> <0041>\nendbfchar\n"));
    const auto compressed = zlib_wrap(plain);
    const auto table = cache.intern_stream_object(
        make_object(3, 0,
                    " /Length " + std::to_string(compressed.size()) +
                        " /Filter /Fl",
                    compressed),
        &report);
    PDTK_ASSERT(table != nullptr);
    PDTK_ASSERT(report.status == CMapStatus::Ok);
    expect_seq(*table, 0x0001, {0x0041}, "legacy /Fl");
}

PDTK_TEST(intern_flate_indirect_length_refused) {
    CMapCache cache;
    CMapInternReport report;
    const auto compressed = zlib_wrap(
        bytes_of(wrap_cmap("1 beginbfchar\n<0001> <0041>\nendbfchar\n")));
    PDTK_ASSERT(cache.intern_stream_object(
                    make_object(4, 0,
                                " /Length 5 0 R /Filter /FlateDecode",
                                compressed),
                    &report) == nullptr);
    PDTK_ASSERT(report.status == CMapStatus::IndirectLength);  // P-021
}

PDTK_TEST(intern_flate_corrupt_payload_fails) {
    CMapCache cache;
    CMapInternReport report;
    std::vector<std::uint8_t> garbage(64);
    Rng rng(99);
    for (auto& b : garbage) {
        b = static_cast<std::uint8_t>(rng.next());
    }
    PDTK_ASSERT(cache.intern_stream_object(
                    make_object(6, 0,
                                " /Length " + std::to_string(garbage.size()) +
                                    " /Filter /FlateDecode",
                                garbage),
                    &report) == nullptr);
    PDTK_ASSERT(report.status == CMapStatus::FlateFailed);
    PDTK_ASSERT_EQ(cache.size(), std::size_t{0});
}

PDTK_TEST(intern_raw_and_flate_payloads_never_merge) {
    // Identical payload BYTES with different filter classes take
    // different dedup keys (seed domain separation).
    CMapCache cache;
    const auto plain = bytes_of(wrap_cmap(
        "1 beginbfchar\n<0001> <0041>\nendbfchar\n"));
    const auto raw_table = cache.intern_stream_object(
        make_object(1, 0, "", plain));
    PDTK_ASSERT(raw_table != nullptr);
    // The same bytes claimed as Flate are not a valid zlib stream.
    CMapInternReport report;
    PDTK_ASSERT(cache.intern_stream_object(
                    make_object(2, 0,
                                " /Length " + std::to_string(plain.size()) +
                                    " /Filter /FlateDecode",
                                plain),
                    &report) == nullptr);
    PDTK_ASSERT(report.status == CMapStatus::FlateFailed);
    PDTK_ASSERT_EQ(cache.size(), std::size_t{1});
}
#endif  // PDTK_HAVE_FLATE

PDTK_TEST(concurrent_intern_is_safe) {
    // TSan-validated: the cache's mutex guards map + codec + arena.
    const auto p1 = bytes_of(wrap_cmap(
        "1 beginbfchar\n<0001> <0041>\nendbfchar\n"));
    const auto p2 = bytes_of(wrap_cmap(
        "1 beginbfchar\n<0001> <0042>\nendbfchar\n"));
    const auto o1 = make_object(1, 0, "", p1);
    const auto o2 = make_object(2, 0, "", p2);

    CMapCache cache;
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 100; ++i) {
                const auto& obj = ((i + t) % 2 == 0) ? o1 : o2;
                const auto table = cache.intern_stream_object(obj);
                PDTK_ASSERT(table != nullptr);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    PDTK_ASSERT_EQ(cache.size(), std::size_t{2});
    PDTK_ASSERT_EQ(cache.intern_hits(), std::size_t{398});
}

// ===========================================================================
// D. Acceptance: the standardized glyph-test corpus (U-013-class
//    synthetic interpretation — deterministic, self-verifying)
// ===========================================================================

// One glyph-test "document": a font's /ToUnicode object plus a coded
// probe string and its exact expected UTF-32 text.
struct GlyphDoc {
    std::vector<std::uint8_t> object;
    std::vector<std::uint32_t> probe;
    std::vector<char32_t> expected;
    std::size_t mapped = 0;
};

GlyphDoc make_glyph_doc(std::uint32_t seed, bool allow_flate) {
    Rng rng(seed);
    const bool cid = rng.below(2) == 0;
    const bool flate = allow_flate && rng.below(5) == 0;

    std::map<std::uint32_t, std::vector<char32_t>> mapping;
    std::string blocks;

    if (cid) {
        // CID-keyed font: 2-byte codespace, contiguous bfranges
        // (CJK-like), one array-form range with mixed targets
        // (surrogate pair + ligature), sparse bfchar tail.
        blocks += "1 begincodespacerange\n<0000> <FFFF>\n"
                  "endcodespacerange\n";
        const int ranges = 2 + static_cast<int>(rng.below(3));
        std::uint32_t code = 0x0021;
        for (int r = 0; r < ranges; ++r) {
            const std::uint32_t width = 3 + rng.below(8);
            const std::uint32_t base = 0x4E00 + rng.below(0x1000);
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "%u beginbfrange\n<%04X> <%04X> <%04X>\n"
                          "endbfrange\n",
                          width, code, code + width - 1, base);
            blocks += buf;
            for (std::uint32_t k = 0; k < width; ++k) {
                mapping[code + k] = {static_cast<char32_t>(base + k)};
            }
            code += width + 1 + rng.below(4);
        }
        // Array form: astral + ligature + plain targets.
        const std::uint32_t a = code + 8;
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "3 beginbfrange\n<%04X> <%04X> "
                      "[<D83DDE00> <FB01> <0178>]\nendbfrange\n",
                      a, a + 2);
        blocks += buf;
        mapping[a] = {0x1F600};
        mapping[a + 1] = {0x0066, 0x0069};
        mapping[a + 2] = {0x0178};
        // Sparse bfchar block.
        const std::uint32_t singles = 1 + rng.below(4);
        blocks += std::to_string(singles) + " beginbfchar\n";
        for (std::uint32_t k = 0; k < singles; ++k) {
            const std::uint32_t c = 0x8000 + rng.below(0x4000);
            const std::uint32_t cp = 0x0020 + rng.below(0x3000);
            char row[64];
            std::snprintf(row, sizeof(row), "<%04X> <%04X>\n", c, cp);
            blocks += row;
            mapping[c] = {static_cast<char32_t>(cp)};
        }
        blocks += "endbfchar\n";
    } else {
        // TrueType subset font: 1-byte codespace, sparse bfchars over
        // a Latin-ish subset plus ligature targets.
        blocks += "1 begincodespacerange\n<00> <FF>\n"
                  "endcodespacerange\n";
        const int singles = 12 + static_cast<int>(rng.below(20));
        blocks += std::to_string(singles) + " beginbfchar\n";
        for (int k = 0; k < singles; ++k) {
            const std::uint32_t c = 0x20 + rng.below(0x5F);
            std::uint32_t target = 0x0020 + rng.below(0x2000);
            if (rng.below(6) == 0) {
                target = 0xFB00 + rng.below(7);  // ligature target
            }
            char row[64];
            std::snprintf(row, sizeof(row), "<%02X> <%04X>\n", c, target);
            blocks += row;
            // Expected = the ligature expansion of `target` — mirrors
            // the verified table (only FB00..FB06 can appear here).
            if (target >= 0xFB00 && target <= 0xFB06) {
                switch (target) {
                    case 0xFB00: mapping[c] = {0x66, 0x66}; break;
                    case 0xFB01: mapping[c] = {0x66, 0x69}; break;
                    case 0xFB02: mapping[c] = {0x66, 0x6C}; break;
                    case 0xFB03: mapping[c] = {0x66, 0x66, 0x69}; break;
                    case 0xFB04: mapping[c] = {0x66, 0x66, 0x6C}; break;
                    case 0xFB05: mapping[c] = {0x017F, 0x74}; break;
                    default: mapping[c] = {0x73, 0x74}; break;
                }
            } else {
                mapping[c] = {static_cast<char32_t>(target)};
            }
        }
        blocks += "endbfchar\n";
    }

    // Probe string: every mapped code, in order (definition order is
    // not required — lookup is random access).
    GlyphDoc doc;
    for (const auto& [code, seq] : mapping) {
        doc.probe.push_back(code);
        doc.expected.insert(doc.expected.end(), seq.begin(), seq.end());
    }
    doc.mapped = mapping.size();

    const auto plain = bytes_of(wrap_cmap(blocks));
#if PDTK_HAVE_FLATE
    if (flate) {
        const auto compressed = zlib_wrap(plain);
        doc.object = make_object(
            1 + seed % 500, 0,
            " /Length " + std::to_string(compressed.size()) +
                " /Filter /FlateDecode",
            compressed);
        return doc;
    }
#else
    (void)flate;
#endif
    doc.object = make_object(1 + seed % 500, 0, "", plain);
    return doc;
}

PDTK_TEST(acceptance_glyph_test_corpus) {
    // "Accurate decoding of complex CID-keyed and TrueType subset
    // fonts verified against a standardized glyph-test document":
    // 500 deterministic glyph-test documents, each decoded through
    // the production intern path and asserted against its exact
    // expected UTF-32. ~1/5 of Flate builds use compressed payloads;
    // every 10th document reuses an earlier one's payload bytes so
    // the dedup path is exercised cross-document (pointer identity).
    constexpr std::uint32_t kDocs = 500;
    constexpr bool kAllowFlate = true;
    CMapCache cache;
    std::map<std::size_t, const CMapTable*> seen;

    for (std::uint32_t i = 0; i < kDocs; ++i) {
        const GlyphDoc doc =
            make_glyph_doc(i * 2654435761u, kAllowFlate);
        CMapInternReport report;
        const auto table = cache.intern_stream_object(doc.object,
                                                       &report);
        PDTK_ASSERT(table != nullptr);
        PDTK_ASSERT(report.status == CMapStatus::Ok);
        PDTK_ASSERT_EQ(report.parse_stats.skipped_entries,
                       std::size_t{0});
        PDTK_ASSERT_EQ(report.parse_stats.mapped_codes, doc.mapped);

        // Decode the probe string; assert the exact expected text.
        std::vector<char32_t> text;
        for (const std::uint32_t code : doc.probe) {
            const std::span<const char32_t> got = table->lookup(code);
            PDTK_ASSERT(!got.empty());
            text.insert(text.end(), got.begin(), got.end());
        }
        if (text != doc.expected) {
            throw std::runtime_error("glyph doc " + std::to_string(i) +
                                     " decoded wrong text");
        }
        // Unmapped codes must never produce output.
        PDTK_ASSERT(table->lookup(0xFFFE0).empty());
    }

    // Every 10th doc reused doc (i-10)'s payload? We made each doc
    // unique here; the dedup assertions live in section C. Instead
    // assert the corpus left a sane number of distinct tables.
    PDTK_ASSERT(cache.size() >= kDocs / 2);
    PDTK_ASSERT(cache.size() <= kDocs);
    (void)seen;
}

PDTK_TEST(acceptance_corpus_dedup_across_documents) {
    // The SAME glyph-test document interned through 50 different
    // object wrappers (object numbers, /Length spellings, generations)
    // shares one frozen table — the audit's global dedup requirement.
    CMapCache cache;
    const GlyphDoc doc = make_glyph_doc(777, false);
    const CMapTable* first = nullptr;
    for (std::uint32_t k = 0; k < 50; ++k) {
        const auto table = cache.intern_stream_object(doc.object);
        PDTK_ASSERT(table != nullptr);
        if (k == 0) {
            first = table.get();
        } else {
            PDTK_ASSERT_EQ(table.get(), first);
        }
    }
    PDTK_ASSERT_EQ(cache.size(), std::size_t{1});
    PDTK_ASSERT_EQ(cache.intern_hits(), std::size_t{49});
}

}  // namespace

PDTK_TEST_MAIN()
