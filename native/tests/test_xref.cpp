// Dual-mode XRef resolver tests (audit issue-1/task-2.2).
//
// Acceptance criterion (audit): "Correctly parses both classical tables
// and compressed streams, verified against a test corpus containing
// corrupt and linearized PDFs." As with task 2.1 (U-013), the corpus is
// deterministic and synthetic — 2,000 generated documents spanning
// classic tables, incremental-update /Prev chains, linearized-style
// layouts, XRef streams (variable /W widths, /Index ranges, type-2
// compressed entries), hybrid /XRefStm files, corrupt newest sections
// and Flate-filtered streams (the P-017 degradation). Every document's
// full expected entry table is known by construction and verified.

#include "test_harness.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "pdftoolkit/errors.hpp"
#include "pdftoolkit/memory/mmap.hpp"
#include "pdftoolkit/parser/xref.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

using pdftoolkit::ErrorCode;
using pdftoolkit::PdfToolkitException;
using pdftoolkit::parser::XRefEntry;
using pdftoolkit::parser::XRefIndex;

std::vector<std::uint8_t> operator""_b(const char* literal, std::size_t n) {
    return std::vector<std::uint8_t>(literal, literal + n);
}

void append(std::vector<std::uint8_t>& out, const char* s) {
    const std::size_t n = std::strlen(s);
    out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(s),
               reinterpret_cast<const std::uint8_t*>(s) + n);
}

void appendf(std::vector<std::uint8_t>& out, const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    append(out, buf);
}

struct Rng {
    std::uint32_t state;
    explicit Rng(std::uint32_t seed) : state(seed * 1664525u + 1013904223u) {}
    std::uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
    std::uint32_t below(std::uint32_t n) { return next() % n; }
};

// --- synthetic document model -------------------------------------------------

struct Doc {
    std::vector<std::uint8_t> bytes;
    std::map<std::uint32_t, XRefEntry> expected;
    std::size_t last_xref_offset = 0;
    bool expect_linear_scan = false;
};

// Appends a stand-alone indirect object and records its expected entry.
void add_object(Doc& doc, std::uint32_t id, std::uint32_t gen) {
    XRefEntry e;
    e.kind = XRefEntry::Kind::InUse;
    e.offset = doc.bytes.size();
    e.generation = gen;
    appendf(doc.bytes, "%u %u obj\n<</N %u>>\nendobj\n", id, gen, id);
    doc.expected[id] = e;
}

// Encodes one variable-width big-endian field.
void put_be(std::vector<std::uint8_t>& out, std::uint64_t v,
            std::uint64_t w) {
    for (std::uint64_t i = w; i-- > 0;) {
        out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF));
    }
}

void verify(const Doc& doc, std::size_t window = 1024) {
    const XRefIndex idx = XRefIndex::from_document(
        std::span<const std::uint8_t>(doc.bytes), window);
    PDTK_ASSERT_EQ(idx.from_linear_scan(), doc.expect_linear_scan);
    for (const auto& [id, want] : doc.expected) {
        const XRefEntry* got = idx.find(id);
        PDTK_ASSERT(got != nullptr);
        PDTK_ASSERT_EQ(static_cast<int>(got->kind),
                       static_cast<int>(want.kind));
        PDTK_ASSERT_EQ(got->offset, want.offset);
        PDTK_ASSERT_EQ(got->generation, want.generation);
        if (want.kind == XRefEntry::Kind::Compressed) {
            PDTK_ASSERT_EQ(got->objstm_object, want.objstm_object);
            PDTK_ASSERT_EQ(got->index_in_objstm, want.index_in_objstm);
        }
    }
}

// Writes the classic table for ids [0..max_id] (free entries where the
// expected map has none).
void write_classic_table(Doc& doc, std::uint32_t max_id) {
    appendf(doc.bytes, "xref\n0 %u\n", max_id + 1);
    for (std::uint32_t id = 0; id <= max_id; ++id) {
        const auto it = doc.expected.find(id);
        if (it != doc.expected.end() &&
            it->second.kind == XRefEntry::Kind::InUse) {
            appendf(doc.bytes, "%010llu %05u n \n",
                    static_cast<unsigned long long>(it->second.offset),
                    it->second.generation);
        } else {
            append(doc.bytes, "0000000000 00000 f \n");
        }
    }
}

// --- corpus document factories --------------------------------------------------

// Classic single-section document.
Doc make_classic(std::uint32_t seed) {
    Rng rng(seed);
    Doc doc;
    const std::uint32_t n = 3 + rng.below(28);
    for (std::uint32_t id = 1; id <= n; ++id) {
        add_object(doc, id, rng.below(3));
    }
    doc.last_xref_offset = doc.bytes.size();
    write_classic_table(doc, n);
    appendf(doc.bytes, "trailer<< /Size %u /Root 1 0 R >>\nstartxref\n"
                       "%llu\n%%EOF\n",
            n + 1,
            static_cast<unsigned long long>(doc.last_xref_offset));
    return doc;
}

// Incremental update: a newer section restates object 1 at a new offset,
// frees object 2, adds a fresh object; /Prev chains to the base section.
Doc make_incremental(std::uint32_t seed) {
    Doc doc = make_classic(seed);
    const std::size_t base_xref = doc.last_xref_offset;
    Rng rng(seed ^ 0x5eedu);
    add_object(doc, 1, 0);  // restated: the later section must win
    const std::uint32_t fresh = 100 + rng.below(50);
    add_object(doc, fresh, 0);

    doc.last_xref_offset = doc.bytes.size();
    appendf(doc.bytes, "xref\n1 2\n%010llu 00000 n \n0000000000 00000 f \n",
            static_cast<unsigned long long>(doc.expected[1].offset));
    appendf(doc.bytes, "%u 1\n%010llu 00000 n \n", fresh,
            static_cast<unsigned long long>(doc.expected[fresh].offset));
    appendf(doc.bytes, "trailer<< /Size %u /Root 1 0 R /Prev %llu >>\n"
                       "startxref\n%llu\n%%EOF\n",
            fresh + 1, static_cast<unsigned long long>(base_xref),
            static_cast<unsigned long long>(doc.last_xref_offset));
    // Object 2 was deleted by the update: a newer free entry sticks.
    XRefEntry free_e;
    free_e.kind = XRefEntry::Kind::Free;
    doc.expected[2] = free_e;
    return doc;
}

// Linearized-style: a first-page xref section early in the file, the
// main section at the end chaining to it via /Prev.
Doc make_linearized(std::uint32_t seed) {
    Doc doc;
    const std::uint32_t later = 5 + (seed % 8);  // 5..12 later objects
    for (std::uint32_t id = 1; id <= 4; ++id) {
        add_object(doc, id, 0);
    }
    const std::size_t early_xref = doc.bytes.size();
    write_classic_table(doc, 4);
    append(doc.bytes, "trailer<< /Size 5 >>\n");
    for (std::uint32_t id = 5; id < 5 + later; ++id) {
        add_object(doc, id, 0);
    }
    doc.last_xref_offset = doc.bytes.size();
    write_classic_table(doc, 4 + later);
    appendf(doc.bytes,
            "trailer<< /Size %u /Root 1 0 R /Prev %llu >>\nstartxref\n"
            "%llu\n%%EOF\n",
            5 + later, static_cast<unsigned long long>(early_xref),
            static_cast<unsigned long long>(doc.last_xref_offset));
    return doc;
}

// XRef stream document: variable /W widths (w1 may be 0 -> default type
// 1), type 0/1/2 rows. The stream object itself is a stand-alone object.
Doc make_xref_stream(std::uint32_t seed) {
    Rng rng(seed);
    Doc doc;
    const std::uint32_t n = 3 + rng.below(20);
    for (std::uint32_t id = 1; id <= n; ++id) {
        add_object(doc, id, rng.below(3));
    }
    const std::uint32_t objstm = n + 1;
    add_object(doc, objstm, 0);
    const std::uint64_t w1 = rng.below(2);
    // With w1 == 0 the type field is absent and EVERY row decodes as
    // type 1 (in use) per the spec — free and compressed entries are
    // unrepresentable. Only a typed stream can carry the type-2 rows.
    const bool typed = w1 > 0;
    if (typed) {
        // Compressed entries (type 2): members of the object stream with
        // no stand-alone body in the file.
        for (std::uint32_t k = 0; k < 3; ++k) {
            XRefEntry e;
            e.kind = XRefEntry::Kind::Compressed;
            e.objstm_object = objstm;
            e.index_in_objstm = k;
            doc.expected[objstm + 1 + k] = e;
        }
    }
    const std::uint32_t total = objstm + 1 + (typed ? 3 : 0);  // ids 0..total-1

    const std::uint64_t w2 = 2 + rng.below(3);
    const std::uint64_t w3 = 1 + rng.below(2);

    std::vector<std::uint8_t> data;
    for (std::uint32_t id = 0; id < total; ++id) {
        const auto it = doc.expected.find(id);
        std::uint64_t type = 1;
        std::uint64_t f2 = 0;
        std::uint64_t f3 = 0;
        if (typed && id == 0) {
            type = 0;
            f3 = 65535;
        } else if (it != doc.expected.end() &&
                   it->second.kind == XRefEntry::Kind::Compressed) {
            type = 2;
            f2 = it->second.objstm_object;
            f3 = it->second.index_in_objstm;
        } else if (it != doc.expected.end()) {
            type = 1;
            f2 = it->second.offset;
            f3 = it->second.generation;
        }
        if (w1 > 0) {
            put_be(data, type, w1);
        }
        put_be(data, f2, w2);
        put_be(data, f3, w3);
    }

    doc.last_xref_offset = doc.bytes.size();
    appendf(doc.bytes,
            "%u 0 obj\n<< /Type /XRef /Size %u /W [%llu %llu %llu] "
            "/Length %llu >>\nstream\n",
            objstm, total, static_cast<unsigned long long>(w1),
            static_cast<unsigned long long>(w2),
            static_cast<unsigned long long>(w3),
            static_cast<unsigned long long>(data.size()));
    doc.bytes.insert(doc.bytes.end(), data.begin(), data.end());
    append(doc.bytes, "\nendstream\nendobj\n");
    appendf(doc.bytes, "startxref\n%llu\n%%EOF\n",
            static_cast<unsigned long long>(doc.last_xref_offset));
    return doc;
}

// Hybrid: classic table + /XRefStm companion stream. The stream encodes
// WRONG offsets for the ids the classic table covers (classic must win)
// and true values everywhere else.
Doc make_hybrid(std::uint32_t seed) {
    Doc doc;
    const std::uint32_t n = 4 + (seed % 5);  // 4..8 classic-covered ids
    for (std::uint32_t id = 1; id <= n; ++id) {
        add_object(doc, id, 0);
    }
    const std::uint32_t objstm = n + 1;
    add_object(doc, objstm, 0);
    XRefEntry ce;
    ce.kind = XRefEntry::Kind::Compressed;
    ce.objstm_object = objstm;
    ce.index_in_objstm = 0;
    doc.expected[objstm + 1] = ce;

    std::vector<std::uint8_t> data;
    for (std::uint32_t id = 0; id <= objstm + 1; ++id) {
        std::uint64_t type = 1;
        std::uint64_t f2 = 0;
        std::uint64_t f3 = 0;
        if (id == 0) {
            type = 0;
            f3 = 65535;
        } else if (id <= n) {
            f2 = 4242;  // deliberately wrong: the classic table must win
        } else if (doc.expected[id].kind == XRefEntry::Kind::Compressed) {
            type = 2;
            f2 = doc.expected[id].objstm_object;
            f3 = doc.expected[id].index_in_objstm;
        } else {
            f2 = doc.expected[id].offset;  // stream-only: true value
        }
        put_be(data, type, 1);
        put_be(data, f2, 2);
        put_be(data, f3, 1);
    }

    const std::size_t stream_at = doc.bytes.size();
    appendf(doc.bytes,
            "%u 0 obj\n<< /Type /XRef /Size %u /W [1 2 1] /Length %llu >>\n"
            "stream\n",
            objstm, objstm + 2,
            static_cast<unsigned long long>(data.size()));
    doc.bytes.insert(doc.bytes.end(), data.begin(), data.end());
    append(doc.bytes, "\nendstream\nendobj\n");

    doc.last_xref_offset = doc.bytes.size();
    appendf(doc.bytes, "xref\n0 %u\n", n + 1);
    append(doc.bytes, "0000000000 65535 f \n");
    for (std::uint32_t id = 1; id <= n; ++id) {
        appendf(doc.bytes, "%010llu 00000 n \n",
                static_cast<unsigned long long>(doc.expected[id].offset));
    }
    appendf(doc.bytes,
            "trailer<< /Size %u /Root 1 0 R /XRefStm %llu >>\n"
            "startxref\n%llu\n%%EOF\n",
            objstm + 2, static_cast<unsigned long long>(stream_at),
            static_cast<unsigned long long>(doc.last_xref_offset));
    return doc;
}

// Corrupt newest section: the table dissolves into garbage — the linear
// scan rebuilds from the (real) object headers.
Doc make_corrupt(std::uint32_t seed) {
    Rng rng(seed);
    Doc doc;
    const std::uint32_t n = 3 + rng.below(15);
    for (std::uint32_t id = 1; id <= n; ++id) {
        add_object(doc, id, 0);
    }
    doc.last_xref_offset = doc.bytes.size();
    append(doc.bytes, "xref\n0 25\n0000000000 65535 f \n");
    append(doc.bytes, "?????????? ????? ???\n");
    append(doc.bytes, "%%garbage%% more junk\n");
    appendf(doc.bytes, "trailer<< /Size %u >>\nstartxref\n%llu\n%%EOF\n",
            n + 1,
            static_cast<unsigned long long>(doc.last_xref_offset));
    doc.expect_linear_scan = true;
    return doc;
}

// Flate-filtered xref stream (P-017): unparseable until task 2.4 — the
// linear scan provides the honest partial index (the stream object's
// own header IS a findable stand-alone object).
Doc make_flate(std::uint32_t seed) {
    Rng rng(seed);
    Doc doc;
    const std::uint32_t n = 3 + rng.below(10);
    for (std::uint32_t id = 1; id <= n; ++id) {
        add_object(doc, id, 0);
    }
    const std::uint32_t stream_id = n + 1;
    const std::size_t stream_at = doc.bytes.size();
    appendf(doc.bytes,
            "%u 0 obj\n<< /Type /XRef /Size %u /W [1 2 1] "
            "/Filter /FlateDecode /Length 20 >>\nstream\n",
            stream_id, n + 2);
    for (int i = 0; i < 20; ++i) {
        doc.bytes.push_back(static_cast<std::uint8_t>(0x78 + i));
    }
    append(doc.bytes, "\nendstream\nendobj\n");
    appendf(doc.bytes, "startxref\n%llu\n%%EOF\n",
            static_cast<unsigned long long>(stream_at));
    XRefEntry se;
    se.kind = XRefEntry::Kind::InUse;
    se.offset = stream_at;
    doc.expected[stream_id] = se;
    doc.expect_linear_scan = true;
    return doc;
}

// ---------------------------------------------------------------------------
// Unit cases
// ---------------------------------------------------------------------------

PDTK_TEST(empty_document_is_invalid_argument) {
    bool threw = false;
    try {
        (void)XRefIndex::from_document(std::span<const std::uint8_t>{});
    } catch (const PdfToolkitException& e) {
        threw = true;
        PDTK_ASSERT_EQ(static_cast<int>(e.code()),
                       static_cast<int>(ErrorCode::InvalidArgument));
    }
    PDTK_ASSERT(threw);
}

PDTK_TEST(classic_single_section) {
    const auto doc = "3 0 obj\n<</A 1>>\nendobj\n"
                     "xref\n0 4\n0000000000 65535 f \n"
                     "0000000000 00000 f \n0000000000 00000 f \n"
                     "0000000000 00000 n \n"
                     "trailer<< /Size 4 /Root 3 0 R >>\nstartxref\n"
                     "24\n%%EOF\n"_b;
    const XRefIndex idx =
        XRefIndex::from_document(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT(!idx.from_linear_scan());
    PDTK_ASSERT_EQ(idx.size(), std::uint32_t{4});
    const XRefEntry* e3 = idx.find(3);
    PDTK_ASSERT(e3 != nullptr);
    PDTK_ASSERT_EQ(static_cast<int>(e3->kind),
                   static_cast<int>(XRefEntry::Kind::InUse));
    PDTK_ASSERT_EQ(e3->offset, std::uint64_t{0});
    PDTK_ASSERT_EQ(e3->generation, std::uint32_t{0});
    PDTK_ASSERT(idx.find(0) != nullptr);  // known free
    PDTK_ASSERT_EQ(static_cast<int>(idx.find(0)->kind),
                   static_cast<int>(XRefEntry::Kind::Free));
    PDTK_ASSERT(idx.find(4) == nullptr);  // beyond /Size
}

PDTK_TEST(classic_multiple_subsections) {
    Doc doc;
    add_object(doc, 1, 0);
    add_object(doc, 5, 0);
    doc.last_xref_offset = doc.bytes.size();
    appendf(doc.bytes, "xref\n1 1\n%010llu 00000 n \n",
            static_cast<unsigned long long>(doc.expected[1].offset));
    appendf(doc.bytes, "5 1\n%010llu 00000 n \n",
            static_cast<unsigned long long>(doc.expected[5].offset));
    appendf(doc.bytes, "trailer<< /Size 6 >>\nstartxref\n%llu\n%%EOF\n",
            static_cast<unsigned long long>(doc.last_xref_offset));
    verify(doc);
}

PDTK_TEST(incremental_update_newest_wins_and_free_sticks) {
    Doc doc;
    add_object(doc, 1, 0);
    add_object(doc, 2, 0);
    const std::size_t base_xref = doc.bytes.size();
    write_classic_table(doc, 2);
    appendf(doc.bytes, "trailer<< /Size 3 >>\nstartxref\n%llu\n%%EOF\n",
            static_cast<unsigned long long>(base_xref));
    // The update: object 1 restated (new physical position wins), object
    // 2 freed, object 3 added.
    add_object(doc, 1, 0);
    add_object(doc, 3, 0);
    doc.last_xref_offset = doc.bytes.size();
    appendf(doc.bytes, "xref\n1 2\n%010llu 00000 n \n0000000000 00000 f \n",
            static_cast<unsigned long long>(doc.expected[1].offset));
    appendf(doc.bytes, "3 1\n%010llu 00000 n \n",
            static_cast<unsigned long long>(doc.expected[3].offset));
    appendf(doc.bytes, "trailer<< /Size 4 /Prev %llu >>\nstartxref\n"
                       "%llu\n%%EOF\n",
            static_cast<unsigned long long>(base_xref),
            static_cast<unsigned long long>(doc.last_xref_offset));
    XRefEntry free_e;
    free_e.kind = XRefEntry::Kind::Free;
    doc.expected[2] = free_e;  // deleted by the update: free sticks
    verify(doc);
}

PDTK_TEST(broken_prev_stops_chain_without_fallback) {
    Doc doc;
    add_object(doc, 1, 0);
    doc.last_xref_offset = doc.bytes.size();
    write_classic_table(doc, 1);
    appendf(doc.bytes,
            "trailer<< /Size 2 /Prev 99999 >>\nstartxref\n%llu\n%%EOF\n",
            static_cast<unsigned long long>(doc.last_xref_offset));
    verify(doc);  // expect_linear_scan stays false
}

PDTK_TEST(prev_cycle_terminates) {
    Doc doc;
    add_object(doc, 1, 0);
    doc.last_xref_offset = doc.bytes.size();
    write_classic_table(doc, 1);
    appendf(doc.bytes,
            "trailer<< /Size 2 /Prev %llu >>\nstartxref\n%llu\n%%EOF\n",
            static_cast<unsigned long long>(doc.last_xref_offset),
            static_cast<unsigned long long>(doc.last_xref_offset));
    verify(doc);  // self-referencing /Prev: must terminate, entries kept
}

PDTK_TEST(corrupt_newest_triggers_linear_scan) {
    Doc doc;
    add_object(doc, 1, 0);
    add_object(doc, 2, 0);
    doc.last_xref_offset = doc.bytes.size();
    append(doc.bytes, "xref\n0 5\n0000000000 65535 f \n@@@garbage@@@\n");
    appendf(doc.bytes, "trailer<< /Size 5 >>\nstartxref\n%llu\n%%EOF\n",
            static_cast<unsigned long long>(doc.last_xref_offset));
    doc.expect_linear_scan = true;
    verify(doc);
}

PDTK_TEST(linear_scan_later_occurrence_wins) {
    // Object 1 restated physically later with no usable xref at all:
    // the later header wins.
    const auto doc =
        "1 0 obj\n<</old>>\nendobj\n1 0 obj\n<</new>>\nendobj\n"
        "startxref\n99\n%%EOF\n"_b;  // 99 points past this small file
    const XRefIndex idx =
        XRefIndex::from_document(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT(idx.from_linear_scan());
    const XRefEntry* e1 = idx.find(1);
    PDTK_ASSERT(e1 != nullptr);
    PDTK_ASSERT_EQ(e1->offset, std::uint64_t{24});
}

PDTK_TEST(linear_scan_rejects_glued_tokens) {
    // "abc12 0 obj" and "7 0 objx" are not object headers.
    const auto doc = "abc12 0 obj\njunk\n7 0 objx\nmore junk\n"
                     "7 0 obj\n<</real>>\nendobj\n"
                     "startxref\n0\n%%EOF\n"_b;
    const XRefIndex idx =
        XRefIndex::from_document(std::span<const std::uint8_t>(doc));
    PDTK_ASSERT(idx.from_linear_scan());
    const XRefEntry* e7 = idx.find(7);
    PDTK_ASSERT(e7 != nullptr);
    PDTK_ASSERT_EQ(e7->offset, std::uint64_t{36});
    PDTK_ASSERT(idx.find(12) == nullptr);
}

PDTK_TEST(xref_stream_type1_and_type2) {
    // /W [1 2 1]; entries: 0=f, 1=n@0, 2=n@15, 3=compressed(4,0).
    std::vector<std::uint8_t> data;
    auto row = [&data](std::uint64_t t, std::uint64_t f2, std::uint64_t f3) {
        put_be(data, t, 1);
        put_be(data, f2, 2);
        put_be(data, f3, 1);
    };
    row(0, 0, 255);
    row(1, 0, 0);
    row(1, 15, 0);
    row(2, 4, 0);
    row(1, 0, 0);  // ids 4 and 5: plain in-use rows (synthetic offsets)
    row(1, 0, 0);

    Doc doc;
    add_object(doc, 1, 0);
    add_object(doc, 2, 0);
    add_object(doc, 4, 0);
    // The rows carry synthetic values; the expected table mirrors them.
    doc.expected[1].offset = 0;
    doc.expected[2].offset = 15;
    XRefEntry ce;
    ce.kind = XRefEntry::Kind::Compressed;
    ce.objstm_object = 4;
    ce.index_in_objstm = 0;
    doc.expected[3] = ce;
    XRefEntry plain;
    plain.kind = XRefEntry::Kind::InUse;
    doc.expected[4] = plain;  // InUse @ 0
    doc.expected[5] = plain;  // InUse @ 0
    const std::size_t stream_at = doc.bytes.size();
    appendf(doc.bytes,
            "5 0 obj\n<< /Type /XRef /Size 6 /W [1 2 1] /Length %llu "
            ">>\nstream\n",
            static_cast<unsigned long long>(data.size()));
    doc.bytes.insert(doc.bytes.end(), data.begin(), data.end());
    append(doc.bytes, "\nendstream\nendobj\n");
    appendf(doc.bytes, "startxref\n%llu\n%%EOF\n",
            static_cast<unsigned long long>(stream_at));
    verify(doc);
}

PDTK_TEST(xref_stream_w1_zero_and_wide_fields) {
    // /W [0 4 2]: type defaults to 1; 4-byte offsets near 2^32.
    std::vector<std::uint8_t> data;
    put_be(data, 4294967295ULL, 4);
    put_be(data, 7, 2);
    put_be(data, 4294967000ULL, 4);
    put_be(data, 9, 2);
    put_be(data, 42, 4);
    put_be(data, 1, 2);

    Doc doc;
    doc.expected[0].kind = XRefEntry::Kind::InUse;
    doc.expected[0].offset = 4294967295ULL;
    doc.expected[0].generation = 7;
    doc.expected[1].kind = XRefEntry::Kind::InUse;
    doc.expected[1].offset = 4294967000ULL;
    doc.expected[1].generation = 9;
    doc.expected[2].kind = XRefEntry::Kind::InUse;
    doc.expected[2].offset = 42;
    doc.expected[2].generation = 1;
    const std::size_t stream_at = doc.bytes.size();  // == 0: no body yet
    appendf(doc.bytes,
            "2 0 obj\n<< /Type /XRef /Size 3 /W [0 4 2] /Length %llu "
            ">>\nstream\n",
            static_cast<unsigned long long>(data.size()));
    doc.bytes.insert(doc.bytes.end(), data.begin(), data.end());
    append(doc.bytes, "\nendstream\nendobj\n");
    appendf(doc.bytes, "startxref\n%llu\n%%EOF\n",
            static_cast<unsigned long long>(stream_at));
    verify(doc);
}

PDTK_TEST(xref_stream_custom_index) {
    // /Index [5 2 9 1]: rows number objects 5, 6 and 9.
    std::vector<std::uint8_t> data;
    auto row = [&data](std::uint64_t t, std::uint64_t f2, std::uint64_t f3) {
        put_be(data, t, 1);
        put_be(data, f2, 2);
        put_be(data, f3, 1);
    };
    row(1, 111, 0);
    row(1, 222, 0);
    row(1, 333, 0);

    Doc doc;
    doc.expected[5].kind = XRefEntry::Kind::InUse;
    doc.expected[5].offset = 111;
    doc.expected[6].kind = XRefEntry::Kind::InUse;
    doc.expected[6].offset = 222;
    doc.expected[9].kind = XRefEntry::Kind::InUse;
    doc.expected[9].offset = 333;
    appendf(doc.bytes,
            "3 0 obj\n<< /Type /XRef /Size 10 /W [1 2 1] "
            "/Index [5 2 9 1] /Length %llu >>\nstream\n",
            static_cast<unsigned long long>(data.size()));
    doc.bytes.insert(doc.bytes.end(), data.begin(), data.end());
    append(doc.bytes, "\nendstream\nendobj\nstartxref\n0\n%%EOF\n");
    verify(doc);
    // Objects the ranges do not mention read as Free.
    const XRefIndex idx = XRefIndex::from_document(
        std::span<const std::uint8_t>(doc.bytes));
    PDTK_ASSERT_EQ(static_cast<int>(idx.find(0)->kind),
                   static_cast<int>(XRefEntry::Kind::Free));
    PDTK_ASSERT_EQ(idx.size(), std::uint32_t{10});
}

PDTK_TEST(xref_stream_chain_to_classic) {
    // Newest = stream; /Prev = classic table. The stream restates
    // object 1 at a new offset (newest wins); classic-only object 2
    // survives at its table-recorded offset.
    std::vector<std::uint8_t> data;
    auto row = [&data](std::uint64_t t, std::uint64_t f2, std::uint64_t f3) {
        put_be(data, t, 1);
        put_be(data, f2, 2);
        put_be(data, f3, 1);
    };
    row(0, 0, 65535);
    row(1, 45, 0);  // object 1 restated by the newer stream
    // The stream mentions only ids 0..1 (/Index [0 2]); object 2 comes
    // from the classic section alone.

    Doc doc;
    add_object(doc, 1, 0);
    add_object(doc, 2, 0);
    const std::size_t classic_xref = doc.bytes.size();
    appendf(doc.bytes, "xref\n0 3\n0000000000 65535 f \n");
    appendf(doc.bytes, "%010llu 00000 n \n",
            static_cast<unsigned long long>(doc.expected[1].offset));
    appendf(doc.bytes, "%010llu 00000 n \n",
            static_cast<unsigned long long>(doc.expected[2].offset));
    append(doc.bytes, "trailer<< /Size 3 >>\n");
    const std::size_t stream_at = doc.bytes.size();
    appendf(doc.bytes,
            "3 0 obj\n<< /Type /XRef /Size 3 /W [1 2 1] /Index [0 2] "
            "/Prev %llu /Length %llu >>\nstream\n",
            static_cast<unsigned long long>(classic_xref),
            static_cast<unsigned long long>(data.size()));
    doc.bytes.insert(doc.bytes.end(), data.begin(), data.end());
    append(doc.bytes, "\nendstream\nendobj\n");
    appendf(doc.bytes, "startxref\n%llu\n%%EOF\n",
            static_cast<unsigned long long>(stream_at));
    doc.expected[1].offset = 45;  // the stream's restatement wins
    verify(doc);
}

PDTK_TEST(flate_stream_falls_back_to_linear_scan) {
    Doc doc;
    add_object(doc, 1, 0);
    const std::size_t stream_at = doc.bytes.size();
    appendf(doc.bytes,
            "2 0 obj\n<< /Type /XRef /Size 3 /W [1 2 1] /Filter "
            "/FlateDecode /Length 8 >>\nstream\nzlibwouldbehere\n"
            "endstream\nendobj\n");
    appendf(doc.bytes, "startxref\n%llu\n%%EOF\n",
            static_cast<unsigned long long>(stream_at));
    XRefEntry se;
    se.kind = XRefEntry::Kind::InUse;
    se.offset = stream_at;
    doc.expected[2] = se;
    doc.expect_linear_scan = true;
    verify(doc);  // P-017: honest degradation
}

PDTK_TEST(no_usable_structure_is_unreadable) {
    const auto doc = "startxref\n5\n%%EOF"_b;  // offset 5 lands on "%%EOF"
    bool threw = false;
    try {
        (void)XRefIndex::from_document(
            std::span<const std::uint8_t>(doc));
    } catch (const PdfToolkitException& e) {
        threw = true;
        PDTK_ASSERT_EQ(static_cast<int>(e.code()),
                       static_cast<int>(ErrorCode::UnreadablePdf));
    }
    PDTK_ASSERT(threw);
}

PDTK_TEST(hostile_object_id_cannot_oom) {
    // A subsection claiming object ids near 2^32 must be refused (the
    // 10M-entry hardening cap), never materialized — and with no
    // findable objects the result is the typed UnreadablePdf.
    const auto doc =
        "xref\n4294967290 5\n0000000000 00000 n \n0000000000 00000 n \n"
        "trailer<< /Size 4294967295 >>\nstartxref\n0\n%%EOF\n"_b;
    bool threw = false;
    try {
        (void)XRefIndex::from_document(
            std::span<const std::uint8_t>(doc));
    } catch (const PdfToolkitException& e) {
        threw = true;
        PDTK_ASSERT_EQ(static_cast<int>(e.code()),
                       static_cast<int>(ErrorCode::UnreadablePdf));
    }
    PDTK_ASSERT(threw);
}

// ---------------------------------------------------------------------------
// Acceptance corpus (2,000 mixed documents)
// ---------------------------------------------------------------------------

PDTK_TEST(acceptance_corpus_two_thousand_documents) {
    constexpr std::uint32_t kCorpus = 2000;
    for (std::uint32_t seed = 0; seed < kCorpus; ++seed) {
        Doc doc;
        if (seed < 500) {
            doc = make_classic(seed);
        } else if (seed < 1000) {
            doc = make_incremental(seed);
        } else if (seed < 1300) {
            doc = make_linearized(seed - 1000);
        } else if (seed < 1700) {
            doc = make_xref_stream(seed - 1300);
        } else if (seed < 1850) {
            doc = make_hybrid(seed - 1700);
        } else if (seed < 1950) {
            doc = make_corrupt(seed - 1850);
        } else {
            doc = make_flate(seed - 1950);
        }
        try {
            verify(doc);
        } catch (const std::exception& e) {
            std::printf("CORPUS FAIL seed=%u mode-block: %s\n", seed, e.what());
            throw;
        }
    }
}

#ifndef _WIN32

class TempPdfFile {
public:
    explicit TempPdfFile(const std::vector<std::uint8_t>& content)
        : path_(std::filesystem::temp_directory_path() /
                ("pdtk_test_xref_" + std::to_string(::getpid()) + "_XXXXXX")) {
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

PDTK_TEST(mmap_backed_index_builds) {
    // Composition proof: the whole locate->resolve chain runs on an
    // MmapHandle's span (the engine's zero-copy ingestion path).
    const Doc doc = make_classic(77);
    const TempPdfFile file(doc.bytes);
    const pdftoolkit::memory::MmapHandle map(file.c_str());
    const XRefIndex idx = XRefIndex::from_document(map.bytes());
    PDTK_ASSERT(!idx.from_linear_scan());
    for (const auto& [id, want] : doc.expected) {
        const XRefEntry* got = idx.find(id);
        PDTK_ASSERT(got != nullptr);
        PDTK_ASSERT_EQ(static_cast<int>(got->kind),
                       static_cast<int>(want.kind));
        PDTK_ASSERT_EQ(got->offset, want.offset);
    }
}

#endif  // !_WIN32

}  // namespace

PDTK_TEST_MAIN()
