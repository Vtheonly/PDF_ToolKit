// Dual-mode XRef resolver (audit issue-1/task-2.2).
//
// Builds the contiguous object->location table from the cross-reference
// structure named by the last startxref (located by task 2.1's backward
// scanner):
//
//   * classical plaintext tables ("xref\n0 N\n<10-digit> <5-digit> n/f"
//     subsections, each followed by a `trailer` dictionary);
//   * PDF 1.5+ /Type /XRef streams (variable-width /W fields, /Index
//     ranges, type 0/1/2 entries) — UNFILTERED only: Flate-filtered
//     streams degrade to the emergency scan until task 2.4 lands the
//     decompressor (P-017);
//   * /Prev chains (incremental updates, linearized first-page tables)
//     with newest-entry-wins merge semantics;
//   * /XRefStm hybrid supplements (classic table + companion stream at
//     the same file version);
//   * the audit's emergency fallback: a corrupt or unparseable NEWEST
//     section rebuilds the index by linearly scanning the file for
//     `N G obj` headers (flagged via XRefIndex::from_linear_scan()).
//
// All parsing is zero-copy position arithmetic over the caller's buffer
// (scan_util.hpp primitives, shared with trailer.cpp since this task).

#include "pdftoolkit/parser/xref.hpp"

#include <algorithm>
#include <cstring>

#include "pdftoolkit/parser/trailer.hpp"
#include "scan_util.hpp"

namespace pdftoolkit::parser {
namespace {

using scan::kNpos;
using scan::is_regular;

// Hardening cap: the contiguous table is indexed by object id, so a
// hostile subsection header or /Index pair claiming id ~2^32 would
// request a ~100 GB allocation. Real PDFs stay far below 10 million
// objects; beyond the cap the table parse refuses (Corrupt) and the
// linear scan skips the object — a hostile file cannot OOM the engine.
constexpr std::uint32_t kMaxObjects = 10'000'000;

// /Prev / /XRefStm chain depth cap (cycle insurance; the visited-list
// check already breaks loops, this bounds memory on pathological files).
constexpr std::size_t kMaxChainLinks = 4096;

enum class SecResult { Ok, Corrupt };

struct ChainLink {
    std::uint64_t prev = 0;
    bool has_prev = false;
    std::uint64_t xrefstm = 0;
    bool has_xrefstm = false;
};

// Shared mutable table under construction. set() implements the
// newest-wins merge (first seen wins because the chain is walked
// newest-first); set_overwrite() implements the linear scan's
// later-occurrence-wins. Both grow the table to `id + 1` slots.
struct Builder {
    std::vector<XRefEntry> entries;
    std::vector<std::uint8_t> written;

    // Returns false when `id` exceeds the hardening cap (caller decides
    // whether that is fatal).
    bool set(std::uint32_t id, const XRefEntry& e) {
        if (id >= kMaxObjects) {
            return false;
        }
        grow(id);
        if (written[id] == 0) {
            entries[id] = e;
            written[id] = 1;
        }
        return true;
    }

    void set_overwrite(std::uint32_t id, const XRefEntry& e) {
        if (id >= kMaxObjects) {
            return;  // best-effort mode: skip hostile ids silently
        }
        grow(id);
        entries[id] = e;
        written[id] = 1;
    }

private:
    void grow(std::uint32_t id) {
        if (id >= entries.size()) {
            entries.resize(id + 1);
            written.resize(id + 1, 0);
        }
    }
};

// Reads a big-endian unsigned integer of `w` bytes at `pos` (1..8).
std::uint64_t read_be(const std::uint8_t* d, std::size_t pos,
                      std::uint64_t w) noexcept {
    std::uint64_t v = 0;
    for (std::uint64_t i = 0; i < w; ++i) {
        v = (v << 8) |
            static_cast<std::uint64_t>(d[pos + static_cast<std::size_t>(i)]);
    }
    return v;
}

// Parses a dictionary VALUE that must be a plain offset integer,
// rejecting indirect references (`4 0 R`) — used for /Prev and
// /XRefStm. Returns true and sets `value` on success.
bool parse_offset_value(const std::uint8_t* d, std::size_t vp,
                         std::size_t ve, std::uint64_t& value) noexcept {
    std::size_t q = scan::skip_ws_and_comments(d, vp, ve);
    if (!scan::parse_u64(d, q, ve, value)) {
        return false;
    }
    // `4 0 R` (broken files) is a reference, not an offset: reject.
    std::size_t s = scan::skip_ws_and_comments(d, q, ve);
    std::size_t probe = s;
    std::uint32_t dummy = 0;
    if (s < ve && scan::is_digit(d[s]) &&
        scan::parse_u32(d, probe, ve, dummy)) {
        std::size_t t = scan::skip_ws_and_comments(d, probe, ve);
        if (t < ve && d[t] == static_cast<std::uint8_t>('R') &&
            (t + 1 >= ve || !is_regular(d[t + 1]))) {
            return false;
        }
    }
    return true;
}

// Extracts /Prev and /XRefStm from a dictionary BODY (positions are
// within the body, brackets excluded).
void extract_chain_keys(const std::uint8_t* d, std::size_t pos,
                        std::size_t end, ChainLink& link) noexcept {
    scan::for_each_dict_entry(
        d, pos, end,
        [&](std::string_view key, std::size_t vp, std::size_t ve) {
            std::uint64_t value = 0;
            if (key == "Prev" && !link.has_prev) {
                if (parse_offset_value(d, vp, ve, value)) {
                    link.has_prev = true;
                    link.prev = value;
                }
            } else if (key == "XRefStm" && !link.has_xrefstm) {
                if (parse_offset_value(d, vp, ve, value)) {
                    link.has_xrefstm = true;
                    link.xrefstm = value;
                }
            }
        });
}

// ---------------------------------------------------------------------------
// Classical plaintext table
// ---------------------------------------------------------------------------

SecResult parse_classic(const std::uint8_t* d, std::size_t size,
                        std::size_t offset, Builder& b,
                        ChainLink& link) noexcept {
    std::size_t q = scan::skip_ws_and_comments(d, offset, size);
    if (!scan::keyword_at(d, q, size, "xref")) {
        return SecResult::Corrupt;
    }
    q += 4;
    while (true) {
        q = scan::skip_ws_and_comments(d, q, size);
        if (q >= size) {
            return SecResult::Corrupt;  // truncated before the trailer
        }
        if (scan::keyword_at(d, q, size, "trailer")) {
            q = scan::skip_ws_and_comments(d, q + 7, size);
            if (q + 1 >= size || d[q] != static_cast<std::uint8_t>('<') ||
                d[q + 1] != static_cast<std::uint8_t>('<')) {
                return SecResult::Corrupt;
            }
            const std::size_t close = scan::matching_dict_close(d, q, size);
            if (close == kNpos) {
                return SecResult::Corrupt;
            }
            extract_chain_keys(d, q + 2, close, link);
            return SecResult::Ok;
        }
        // Subsection header: `<start> <count>`
        std::uint32_t start = 0;
        std::uint32_t count = 0;
        if (!scan::parse_u32(d, q, size, start)) {
            return SecResult::Corrupt;
        }
        std::size_t r = scan::skip_ws_and_comments(d, q, size);
        if (r == q) {
            return SecResult::Corrupt;
        }
        if (!scan::parse_u32(d, r, size, count)) {
            return SecResult::Corrupt;
        }
        if (start > kMaxObjects ||
            (count != 0 && count > kMaxObjects - start)) {
            return SecResult::Corrupt;  // ids would pass the hardening cap
        }
        q = r;
        for (std::uint32_t i = 0; i < count; ++i) {
            q = scan::skip_ws_and_comments(d, q, size);
            std::uint64_t field1 = 0;
            std::uint32_t gen = 0;
            if (!scan::parse_u64(d, q, size, field1)) {
                return SecResult::Corrupt;
            }
            std::size_t s = scan::skip_ws_and_comments(d, q, size);
            if (s == q) {
                return SecResult::Corrupt;
            }
            if (!scan::parse_u32(d, s, size, gen)) {
                return SecResult::Corrupt;
            }
            q = scan::skip_ws_and_comments(d, s, size);
            if (q >= size || (d[q] != static_cast<std::uint8_t>('n') &&
                              d[q] != static_cast<std::uint8_t>('f'))) {
                return SecResult::Corrupt;
            }
            if (q + 1 < size && is_regular(d[q + 1])) {
                return SecResult::Corrupt;  // type char must be terminated
            }
            XRefEntry e;
            e.offset = field1;
            e.generation = gen;
            e.kind = d[q] == static_cast<std::uint8_t>('n')
                         ? XRefEntry::Kind::InUse
                         : XRefEntry::Kind::Free;
            if (!b.set(start + i, e)) {
                return SecResult::Corrupt;  // id beyond kMaxObjects
            }
            ++q;
        }
    }
}

// ---------------------------------------------------------------------------
// XRef stream (PDF 1.5+, unfiltered)
// ---------------------------------------------------------------------------

SecResult parse_xref_stream(const std::uint8_t* d, std::size_t size,
                            std::size_t offset, Builder& b,
                            ChainLink& link) noexcept {
    // Indirect object header: `N G obj`
    std::size_t q = scan::skip_ws_and_comments(d, offset, size);
    std::uint32_t objnum = 0;
    std::uint32_t gen = 0;
    if (!scan::parse_u32(d, q, size, objnum)) {
        return SecResult::Corrupt;
    }
    std::size_t r = scan::skip_ws_and_comments(d, q, size);
    if (r == q) {
        return SecResult::Corrupt;
    }
    if (!scan::parse_u32(d, r, size, gen)) {
        return SecResult::Corrupt;
    }
    q = scan::skip_ws_and_comments(d, r, size);
    if (q == r || !scan::keyword_at(d, q, size, "obj")) {
        return SecResult::Corrupt;
    }
    q = scan::skip_ws_and_comments(d, q + 3, size);

    // Stream dictionary.
    if (q + 1 >= size || d[q] != static_cast<std::uint8_t>('<') ||
        d[q + 1] != static_cast<std::uint8_t>('<')) {
        return SecResult::Corrupt;
    }
    const std::size_t close = scan::matching_dict_close(d, q, size);
    if (close == kNpos) {
        return SecResult::Corrupt;
    }

    std::vector<std::uint64_t> w;
    std::vector<std::uint64_t> index_pairs;
    std::uint64_t size_n = 0;
    std::uint64_t length = 0;
    bool has_size = false;
    bool has_length = false;
    bool type_bad = false;
    bool filtered = false;
    scan::for_each_dict_entry(
        d, q + 2, close,
        [&](std::string_view key, std::size_t vp, std::size_t ve) {
            if (key == "W") {
                std::size_t p = vp;
                w.clear();
                (void)scan::parse_array_u64(d, p, ve, w);
            } else if (key == "Index") {
                std::size_t p = vp;
                index_pairs.clear();
                (void)scan::parse_array_u64(d, p, ve, index_pairs);
            } else if (key == "Size" && !has_size) {
                std::size_t p = scan::skip_ws_and_comments(d, vp, ve);
                if (scan::parse_u64(d, p, ve, size_n)) {
                    has_size = true;
                }
            } else if (key == "Length" && !has_length) {
                std::size_t p = scan::skip_ws_and_comments(d, vp, ve);
                if (scan::parse_u64(d, p, ve, length)) {
                    has_length = true;
                }
                // An indirect /Length (`4 0 R`) cannot be resolved before
                // the index exists (chicken-and-egg): the byte-count
                // bounds check below replaces it.
            } else if (key == "Type") {
                std::size_t p = scan::skip_ws_and_comments(d, vp, ve);
                if (p >= ve || d[p] != static_cast<std::uint8_t>('/')) {
                    type_bad = true;  // /Type's value must be a name
                } else {
                    const std::size_t len = scan::name_length(d, p + 1, ve);
                    if (len != 4 ||
                        std::memcmp(d + p + 1, "XRef", 4) != 0) {
                        type_bad = true;
                    }
                }
            } else if (key == "Filter") {
                // A name, or an array of names. Any non-empty filter
                // means the stream bytes are not raw xref rows: Flate
                // arrives with task 2.4 (P-017).
                std::size_t p = scan::skip_ws_and_comments(d, vp, ve);
                if (p < ve && d[p] == static_cast<std::uint8_t>('[')) {
                    std::size_t a = p + 1;
                    while (a < ve) {
                        a = scan::skip_ws_and_comments(d, a, ve);
                        if (a >= ve || d[a] == static_cast<std::uint8_t>(']')) {
                            break;
                        }
                        if (d[a] == static_cast<std::uint8_t>('/')) {
                            if (scan::name_length(d, a + 1, ve) > 0) {
                                filtered = true;
                            }
                            a += 1 + scan::name_length(d, a + 1, ve);
                        } else {
                            ++a;
                        }
                    }
                } else if (p < ve && d[p] == static_cast<std::uint8_t>('/')) {
                    filtered = scan::name_length(d, p + 1, ve) > 0;
                }
            } else if (key == "Prev" || key == "XRefStm") {
                std::uint64_t value = 0;
                if (parse_offset_value(d, vp, ve, value)) {
                    if (key == "Prev" && !link.has_prev) {
                        link.has_prev = true;
                        link.prev = value;
                    } else if (key == "XRefStm" && !link.has_xrefstm) {
                        link.has_xrefstm = true;
                        link.xrefstm = value;
                    }
                }
            }
        });

    if (type_bad) {
        return SecResult::Corrupt;
    }
    if (filtered) {
        // Not corrupt in the strict sense — unparseable until task 2.4.
        // The caller's newest-section rule routes this to the linear
        // scan; older sections simply stop the chain.
        return SecResult::Corrupt;
    }
    if (w.size() != 3) {
        return SecResult::Corrupt;  // /W is required, exactly 3 widths
    }
    for (const std::uint64_t width : w) {
        if (width > 8) {
            return SecResult::Corrupt;
        }
    }
    const std::uint64_t row = w[0] + w[1] + w[2];
    if (row == 0) {
        return SecResult::Corrupt;
    }

    // Stream data: after `stream` exactly one EOL (\r\n | \n | \r).
    q = scan::skip_ws_and_comments(d, close + 2, size);
    if (!scan::keyword_at(d, q, size, "stream")) {
        return SecResult::Corrupt;
    }
    q += 6;
    if (q < size && d[q] == static_cast<std::uint8_t>('\r')) {
        ++q;
    }
    if (q < size && d[q] == static_cast<std::uint8_t>('\n')) {
        ++q;
    }
    const std::size_t data_start = q;
    if (data_start > size) {
        return SecResult::Corrupt;
    }

    // Object numbering: /Index pairs (default [0 /Size]).
    if (index_pairs.empty()) {
        if (!has_size) {
            return SecResult::Corrupt;
        }
        index_pairs.push_back(0);
        index_pairs.push_back(size_n);
    }
    if (index_pairs.size() % 2 != 0) {
        return SecResult::Corrupt;
    }
    std::uint64_t total = 0;
    for (std::size_t i = 0; i + 1 < index_pairs.size(); i += 2) {
        const std::uint64_t objnum0 = index_pairs[i];
        const std::uint64_t cnt = index_pairs[i + 1];
        if (objnum0 > kMaxObjects || cnt > size) {
            return SecResult::Corrupt;
        }
        if (cnt > 0 && objnum0 + cnt - 1 > kMaxObjects) {
            return SecResult::Corrupt;  // ids would pass the hardening cap
        }
        // Saturating total: an entry count can never exceed the bytes
        // that encode it.
        if (cnt > size - total) {
            return SecResult::Corrupt;
        }
        total += cnt;
    }
    if (total > (size - data_start) / row) {
        return SecResult::Corrupt;  // not enough stream bytes for the rows
    }
    if (has_length && length < total * row) {
        return SecResult::Corrupt;  // contradicts its own /Length
    }

    // Decode the variable-width rows.
    std::size_t p = data_start;
    for (std::size_t i = 0; i + 1 < index_pairs.size(); i += 2) {
        const std::uint64_t objnum0 = index_pairs[i];
        const std::uint64_t cnt = index_pairs[i + 1];
        for (std::uint64_t j = 0; j < cnt; ++j) {
            std::uint64_t type = 1;  // w1 == 0: default type is "in use"
            if (w[0] > 0) {
                type = read_be(d, p, w[0]);
            }
            p += static_cast<std::size_t>(w[0]);
            std::uint64_t f2 = 0;
            std::uint64_t f3 = 0;
            if (w[1] > 0) {
                f2 = read_be(d, p, w[1]);
            }
            p += static_cast<std::size_t>(w[1]);
            if (w[2] > 0) {
                f3 = read_be(d, p, w[2]);
            }
            p += static_cast<std::size_t>(w[2]);

            XRefEntry e;
            if (type == 0) {
                e.kind = XRefEntry::Kind::Free;
                e.offset = f2;
                e.generation = static_cast<std::uint32_t>(f3);
            } else if (type == 1) {
                e.kind = XRefEntry::Kind::InUse;
                e.offset = f2;
                e.generation = static_cast<std::uint32_t>(f3);
            } else if (type == 2) {
                e.kind = XRefEntry::Kind::Compressed;
                e.objstm_object = static_cast<std::uint32_t>(f2);
                e.index_in_objstm = static_cast<std::uint32_t>(f3);
            } else {
                return SecResult::Corrupt;  // spec types are 0, 1, 2 only
            }
            if (!b.set(static_cast<std::uint32_t>(objnum0 + j), e)) {
                return SecResult::Corrupt;
            }
        }
    }
    return SecResult::Ok;
}

// ---------------------------------------------------------------------------
// Emergency fallback (audit task 2.2 step 5)
// ---------------------------------------------------------------------------

// Rebuilds the index by scanning the whole document for `N G obj`
// headers at token boundaries. Later occurrences overwrite earlier ones
// (incremental updates restate objects physically later in the file).
// Objects compressed inside object streams have no header in the file
// and are invisible to this scan — the reduced trust is flagged.
void linear_scan(const std::uint8_t* d, std::size_t size,
                 Builder& b) noexcept {
    std::size_t q = 0;
    while (q < size) {
        if (scan::is_digit(d[q]) &&
            (q == 0 || !is_regular(d[q - 1]))) {
            std::size_t p = q;
            std::uint32_t objnum = 0;
            std::uint32_t gen = 0;
            if (scan::parse_u32(d, p, size, objnum)) {
                std::size_t r = scan::skip_ws_and_comments(d, p, size);
                if (r > p && scan::parse_u32(d, r, size, gen)) {
                    std::size_t s = scan::skip_ws_and_comments(d, r, size);
                    if (s > r && scan::keyword_at(d, s, size, "obj")) {
                        XRefEntry e;
                        e.kind = XRefEntry::Kind::InUse;
                        e.offset = q;
                        e.generation = gen;
                        b.set_overwrite(objnum, e);
                        q = s + 3;
                        continue;
                    }
                }
            }
            while (q < size && scan::is_digit(d[q])) {
                ++q;  // skip the digit run; sub-runs fail the boundary rule
            }
        } else {
            ++q;
        }
    }
}

}  // namespace

XRefIndex XRefIndex::from_document(std::span<const std::uint8_t> bytes,
                                   std::size_t tail_window) {
    // locate_startxref validates emptiness/window and throws the
    // no-startxref UnreadablePdf — both propagate unchanged.
    const TrailerInfo info = locate_startxref(bytes, tail_window);
    const std::uint8_t* d = bytes.data();
    const std::size_t size = bytes.size();

    Builder b;
    std::vector<std::uint64_t> visited;
    std::uint64_t at = info.xref_offset;
    bool any_ok = false;
    while (at < size && visited.size() < kMaxChainLinks &&
           std::find(visited.begin(), visited.end(), at) == visited.end()) {
        visited.push_back(at);
        ChainLink link;
        const std::size_t probe = scan::skip_ws_and_comments(d, at, size);
        const bool classic = scan::keyword_at(d, probe, size, "xref");
        const SecResult r = classic
                                ? parse_classic(d, size, at, b, link)
                                : parse_xref_stream(d, size, at, b, link);
        if (r != SecResult::Ok) {
            break;  // newest corrupt (-> linear scan) or older corrupt
                    // (-> stop the chain, newer entries stay)
        }
        any_ok = true;
        // Hybrid-reference file: a classic section's /XRefStm names a
        // companion stream holding the same version's entries. Classic
        // entries were written first, so first-seen-wins gives the
        // classic table precedence, exactly as older readers see it.
        if (classic && link.has_xrefstm && link.xrefstm < size &&
            std::find(visited.begin(), visited.end(), link.xrefstm) ==
                visited.end() &&
            visited.size() < kMaxChainLinks) {
            visited.push_back(link.xrefstm);
            ChainLink stream_link;
            (void)parse_xref_stream(d, size, static_cast<std::size_t>(
                                                 link.xrefstm),
                                    b, stream_link);
            // A failed companion stream leaves the classic section
            // standing — not a corruption of the chain.
        }
        if (link.has_prev && link.prev < size) {
            at = link.prev;
            continue;
        }
        break;
    }

    XRefIndex index;
    if (!any_ok) {
        // The newest section is corrupt, unparseable (e.g. Flate-filtered
        // xref stream, P-017) or points outside the file: rebuild by the
        // emergency linear scan. The flag carries the reduced trust.
        Builder fresh;
        linear_scan(d, size, fresh);
        if (fresh.entries.empty()) {
            throw PdfToolkitException(
                ErrorCode::UnreadablePdf,
                "XRefIndex: no cross-reference and no objects found");
        }
        index.entries_ = std::move(fresh.entries);
        index.written_ = std::move(fresh.written);
        index.from_linear_scan_ = true;
        return index;
    }
    index.entries_ = std::move(b.entries);
    index.written_ = std::move(b.written);
    return index;
}

const XRefEntry* XRefIndex::find(std::uint32_t object) const noexcept {
    if (object >= entries_.size()) {
        return nullptr;
    }
    return &entries_[object];
}

}  // namespace pdftoolkit::parser
