// stream_util.hpp — shared indirect-stream-object scanning primitives.
//
// INTERNAL header (lives under src/, never installed, never included
// from public headers). Extracted from xref.cpp (task 2.2) when task
// 3.1's CMap parser needed the same `N G obj <<...>> stream` anatomy —
// extend, never fork (AGENTS.md §1.6; the same extraction precedent as
// scan_util.hpp in task 2.2). Task 3.2/3.3's content-stream consumers
// are the expected third users.
//
// Owns the parts of a stream object that every consumer needs:
//   * the `N G obj` indirect-object header;
//   * direct-integer value parsing that refuses indirect references
//     (the /Length chicken-and-egg, P-021);
//   * /Filter classification (exactly-one-FlateDecode vs anything
//     else, including the legacy /Fl abbreviation);
//   * /DecodeParms predictor detection (the P-020 degradation rule);
//   * the `stream` keyword + single-EOL payload location rule.

#ifndef PDFTOOLKIT_PARSER_STREAM_UTIL_HPP
#define PDFTOOLKIT_PARSER_STREAM_UTIL_HPP

#include <cstdint>
#include <cstring>

#include "scan_util.hpp"

namespace pdftoolkit::parser::stream {

using scan::kNpos;

// --- indirect object header ---------------------------------------------------

// Parses `N G obj` starting at `offset` (leading whitespace/comments
// allowed). Returns the position just past `obj` (before any following
// whitespace — callers skip as needed), or kNpos when the header is
// malformed. The object and generation numbers are consumed but not
// returned: no current consumer needs them (the xref resolver already
// knows both; the CMap cache keys on content, not identity).
inline std::size_t parse_indirect_header(const std::uint8_t* d,
                                         std::size_t size,
                                         std::size_t offset) noexcept {
    std::size_t q = scan::skip_ws_and_comments(d, offset, size);
    std::uint32_t objnum = 0;
    if (!scan::parse_u32(d, q, size, objnum)) {
        return kNpos;
    }
    std::size_t r = scan::skip_ws_and_comments(d, q, size);
    if (r == q) {
        return kNpos;
    }
    std::uint32_t gen = 0;
    if (!scan::parse_u32(d, r, size, gen)) {
        return kNpos;
    }
    q = scan::skip_ws_and_comments(d, r, size);
    if (q == r || !scan::keyword_at(d, q, size, "obj")) {
        return kNpos;
    }
    return q + 3;
}

// --- direct-integer values ------------------------------------------------------

// Parses an integer value in the region [vp, ve) that must be a DIRECT
// number (an offset, a byte count). A trailing `G R` reference form
// (`4 0 R`) is rejected: an indirect /Length cannot be resolved before
// the object table exists, and pre-task-2.4 code half-parsed the object
// number as the byte count (P-021). Returns true and sets `value` on
// success; `+`-signed values are legal PDF.
inline bool parse_direct_integer(const std::uint8_t* d, std::size_t vp,
                                 std::size_t ve,
                                 std::uint64_t& value) noexcept {
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
            (t + 1 >= ve || !scan::is_regular(d[t + 1]))) {
            return false;
        }
    }
    return true;
}

// --- /Filter classification -------------------------------------------------------

// What a stream dictionary's /Filter entry says. `flate_only` is the
// exactly-one-FlateDecode case (the task-2.4 first-class path): a single
// name, either the spelled-out form or the legacy /Fl abbreviation.
struct FilterClass {
    bool has_filter = false;   // any /Filter entry was present
    bool flate_only = false;   // ... and it is exactly one FlateDecode
};

inline bool is_flate_filter_name(const std::uint8_t* base, std::size_t pos,
                                 std::size_t end) noexcept {
    // `pos` sits just past the '/'; FlateDecode or the legacy Fl.
    const std::size_t len = scan::name_length(base, pos, end);
    if (len == 11 &&
        std::memcmp(base + pos, "FlateDecode", 11) == 0) {
        return true;
    }
    return len == 2 && std::memcmp(base + pos, "Fl", 2) == 0;
}

// Classifies the /Filter VALUE region [vp, ve): a name, or an array of
// names. An empty/absent-looking region yields {false, false}.
inline FilterClass classify_filter(const std::uint8_t* d, std::size_t vp,
                                   std::size_t ve) noexcept {
    FilterClass out;
    std::size_t p = scan::skip_ws_and_comments(d, vp, ve);
    if (p < ve && d[p] == static_cast<std::uint8_t>('[')) {
        std::size_t a = p + 1;
        int flate_names = 0;
        int names = 0;
        while (a < ve) {
            a = scan::skip_ws_and_comments(d, a, ve);
            if (a >= ve || d[a] == static_cast<std::uint8_t>(']')) {
                break;
            }
            if (d[a] == static_cast<std::uint8_t>('/')) {
                const std::size_t len = scan::name_length(d, a + 1, ve);
                if (len > 0) {
                    ++names;
                    flate_names += is_flate_filter_name(d, a + 1, ve) ? 1 : 0;
                }
                a += 1 + len;
            } else {
                ++a;
            }
        }
        if (names > 0) {
            out.has_filter = true;
            out.flate_only = (names == 1 && flate_names == 1);
        }
    } else if (p < ve && d[p] == static_cast<std::uint8_t>('/')) {
        const std::size_t len = scan::name_length(d, p + 1, ve);
        if (len > 0) {
            out.has_filter = true;
            out.flate_only = is_flate_filter_name(d, p + 1, ve);
        }
    }
    return out;
}

// --- /DecodeParms predictor detection ------------------------------------------------

// True when the /DecodeParms VALUE region [vp, ve) demands row
// un-prediction this engine does not implement: an array form (filter
// pipeline parms) or a dictionary with /Predictor >= 2 (PNG 12 / TIFF
// 2). The P-020 rule: silently ignoring a predictor would mis-decode
// every row — degrade honestly instead.
inline bool parms_demand_predictor(const std::uint8_t* d, std::size_t vp,
                                   std::size_t ve) noexcept {
    std::size_t p = scan::skip_ws_and_comments(d, vp, ve);
    if (p < ve && d[p] == static_cast<std::uint8_t>('[')) {
        return true;  // pipeline parms: unsupported
    }
    if (p < ve && d[p] == static_cast<std::uint8_t>('<') && p + 1 < ve &&
        d[p + 1] == static_cast<std::uint8_t>('<')) {
        const std::size_t close = scan::matching_dict_close(d, p, ve);
        if (close != kNpos) {
            bool predictor = false;
            scan::for_each_dict_entry(
                d, p + 2, close,
                [&](std::string_view key, std::size_t pvp, std::size_t pve) {
                    if (key == "Predictor") {
                        std::size_t pp = scan::skip_ws_and_comments(d, pvp, pve);
                        std::uint64_t value = 0;
                        if (scan::parse_u64(d, pp, pve, value) && value >= 2) {
                            predictor = true;
                        }
                    }
                });
            return predictor;
        }
    }
    return false;
}

// --- stream payload location -------------------------------------------------------

// Locates the stream payload start: after the dictionary's closing
// `>>` (at `dict_close`), skip whitespace/comments, require the
// `stream` keyword, then skip exactly one EOL (\r\n | \n | \r).
// Returns the payload start, or kNpos when malformed.
inline std::size_t locate_stream_data(const std::uint8_t* d, std::size_t size,
                                      std::size_t dict_close) noexcept {
    std::size_t q = scan::skip_ws_and_comments(d, dict_close, size);
    if (!scan::keyword_at(d, q, size, "stream")) {
        return kNpos;
    }
    q += 6;
    if (q < size && d[q] == static_cast<std::uint8_t>('\r')) {
        ++q;
    }
    if (q < size && d[q] == static_cast<std::uint8_t>('\n')) {
        ++q;
    }
    return q <= size ? q : kNpos;
}

}  // namespace pdftoolkit::parser::stream

#endif  // PDFTOOLKIT_PARSER_STREAM_UTIL_HPP
