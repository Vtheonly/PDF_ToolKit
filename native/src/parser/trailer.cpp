// Backward startxref / trailer scanner (audit issue-1/task-2.1).
//
// Reading a PDF starts at the END: the last `startxref` names the
// cross-reference section, and the classic `trailer` dictionary (with
// /Root) sits immediately before it. This scanner works entirely on the
// caller's buffer (zero-copy; the buffer is normally an MmapHandle's
// span) and searches the tail backward with SIMD needle matching
// (`_mm256_cmpeq_epi8` against 's'), exactly as the audit prescribes.
//
// Portability: the AVX2 search is compiled with a function-target
// attribute and dispatched at runtime (`__builtin_cpu_supports`), so the
// same binary runs on non-AVX2 hosts (scalar fallback), under Debug
// builds (no -march=native) and on non-x86 / MSVC targets. The scalar
// and SIMD paths are differential-tested in native/tests/test_trailer.cpp.

#include "pdftoolkit/parser/trailer.hpp"

#include <cstring>

#include "scan_util.hpp"  // shared parser primitives (internal, task 2.2 reuse)

#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
#define PDTK_TRAILER_AVX2_RUNTIME 1
#else
// MSVC has no function-target attributes (would need /arch:AVX2 globally)
// and non-x86 targets get the NEON treatment with audit task 4.3.
#define PDTK_TRAILER_AVX2_RUNTIME 0
#endif

namespace pdftoolkit::parser {
namespace {

using scan::kNpos;
using scan::is_ws;
using scan::is_delimiter;
using scan::is_regular;
using scan::matching_dict_close;

bool g_force_scalar = false;

// ---------------------------------------------------------------------------
// Backward needle search: highest 's' position in [lo, limit), or kNpos.
// ---------------------------------------------------------------------------

std::size_t highest_s_below_scalar(const std::uint8_t* data,
                                   std::size_t lo,
                                   std::size_t limit) noexcept {
    std::size_t p = limit;
    while (p > lo) {
        --p;
        if (data[p] == static_cast<std::uint8_t>('s')) {
            return p;
        }
    }
    return kNpos;
}

#if PDTK_TRAILER_AVX2_RUNTIME
// 32-byte chunks from the top of [lo, limit) downward; within a chunk,
// candidate bits are examined from the highest position down, so the
// first hit reported is the highest 's' below `limit`.
__attribute__((target("avx2"))) std::size_t highest_s_below_avx2(
    const std::uint8_t* data, std::size_t lo, std::size_t limit) noexcept {
    std::size_t c_end = limit;
    while (c_end > lo) {
        const std::size_t width =
            (c_end - lo >= 32) ? std::size_t{32} : c_end - lo;
        const std::size_t c_start = c_end - width;
        __m256i chunk;
        if (width == 32) {
            chunk = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(data + c_start));
        } else {
            // Partial chunk: never read past `limit`/the buffer end —
            // an unmasked 32-byte load would touch bytes the caller
            // does not own (heap overread under ASan, SIGBUS past an
            // mmap). Copy the valid bytes into a zeroed staging buffer;
            // the bit mask below then ignores the zero tail anyway.
            alignas(32) std::uint8_t staging[32] = {};
            std::memcpy(staging, data + c_start, width);
            chunk = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(staging));
        }
        const __m256i hits =
            _mm256_cmpeq_epi8(chunk, _mm256_set1_epi8(static_cast<char>('s')));
        const unsigned bits =
            static_cast<unsigned>(_mm256_movemask_epi8(hits));
        const unsigned mask =
            (width == 32) ? ~0U : ((1U << width) - 1U);
        const unsigned valid = bits & mask;
        if (valid != 0) {
            const int bit = 31 - __builtin_clz(valid);
            return c_start + static_cast<std::size_t>(bit);
        }
        c_end = c_start;
    }
    return kNpos;
}
#endif

std::size_t highest_s_below(const std::uint8_t* data, std::size_t lo,
                            std::size_t limit) noexcept {
#if PDTK_TRAILER_AVX2_RUNTIME
    if (!g_force_scalar && __builtin_cpu_supports("avx2")) {
        return highest_s_below_avx2(data, lo, limit);
    }
#endif
    return highest_s_below_scalar(data, lo, limit);
}

// ---------------------------------------------------------------------------
// Trailer dictionary extraction (best-effort; never produces wrong data).
// ---------------------------------------------------------------------------

// Extracts /Root and /Prev from the dictionary body [pos, end) via the
// shared string-aware, nesting-aware walker (scan_util.hpp) — decoys
// inside strings or nested dictionaries are invisible by construction.
// First occurrence of each key wins; malformed values are recorded as
// absent, never as wrong data.
void extract_root_and_prev(const std::uint8_t* d, std::size_t pos,
                           std::size_t end, TrailerInfo& info) noexcept {
    scan::for_each_dict_entry(
        d, pos, end,
        [&](std::string_view key, std::size_t vp, std::size_t ve) {
            if (key == "Root" && !info.has_root) {
                // `/Root <obj> <gen> R`, each token separated by at least
                // one whitespace (PDF token syntax).
                std::size_t q = scan::skip_ws_and_comments(d, vp, ve);
                std::uint32_t obj = 0;
                std::uint32_t gen = 0;
                if (!scan::parse_u32(d, q, ve, obj)) {
                    return;
                }
                const std::size_t after_obj = q;
                std::size_t s = scan::skip_ws_and_comments(d, q, ve);
                if (s == after_obj) {
                    return;  // tokens must be whitespace-separated
                }
                if (!scan::parse_u32(d, s, ve, gen)) {
                    return;
                }
                const std::size_t after_gen = s;
                std::size_t t = scan::skip_ws_and_comments(d, s, ve);
                if (t == after_gen || t >= ve ||
                    d[t] != static_cast<std::uint8_t>('R') ||
                    (t + 1 < ve && is_regular(d[t + 1]))) {
                    return;  // must be the keyword R, terminated
                }
                info.has_root = true;
                info.root_object = obj;
                info.root_generation = gen;
            } else if (key == "Prev" && !info.has_prev) {
                std::size_t q = scan::skip_ws_and_comments(d, vp, ve);
                std::uint64_t prev = 0;
                if (!scan::parse_u64(d, q, ve, prev)) {
                    return;
                }
                // `/Prev` must be a plain integer; an indirect reference
                // (`/Prev 4 0 R`, broken files) is recorded as absent.
                std::size_t s = scan::skip_ws_and_comments(d, q, ve);
                std::size_t probe = s;
                std::uint32_t dummy = 0;
                if (s < ve && scan::is_digit(d[s]) &&
                    scan::parse_u32(d, probe, ve, dummy)) {
                    std::size_t t = scan::skip_ws_and_comments(d, probe, ve);
                    if (t < ve && d[t] == static_cast<std::uint8_t>('R') &&
                        (t + 1 >= ve || !is_regular(d[t + 1]))) {
                        return;  // indirect reference, not an offset
                    }
                }
                info.has_prev = true;
                info.prev_offset = prev;
            }
        });
}

// Locates the classic trailer dictionary ending just before the
// `startxref` keyword at `keyword_pos` and fills `info.dict`,
// `info.has_root` and `info.has_prev`. Best-effort by contract: on any
// structural surprise it simply leaves the dictionary fields empty.
void extract_trailer(const std::uint8_t* d, std::size_t keyword_pos,
                     std::size_t size, TrailerInfo& info) noexcept {
    // 1. Skip whitespace backward from the keyword; the dictionary's
    //    closing ">>" must sit there in a classic trailer.
    std::size_t p = keyword_pos;
    while (p > 0 && is_ws(d[p - 1])) {
        --p;
    }
    if (p < 2 || d[p - 1] != static_cast<std::uint8_t>('>') ||
        d[p - 2] != static_cast<std::uint8_t>('>')) {
        return;  // XRef-stream layout or malformed: no classic trailer
    }
    const std::size_t close = p - 2;  // first '>' of the closing ">"

    // 2. Scan backward for the `trailer` keyword within a generous bound
    //    (real trailer dictionaries, /ID strings included, are far below
    //    this; the bound only stops pathological garbage from scanning
    //    the whole file). A decoy ">>" from an inner dictionary is
    //    impossible here: the close above is the OUTERMOST ">>" before
    //    the keyword, and step 3 re-verifies the pair forward.
    constexpr std::size_t kTrailerBound = 65536;
    const std::size_t floor =
        close > kTrailerBound ? close - kTrailerBound : 0;
    std::size_t t = close;
    bool found = false;
    while (t >= floor + 7 && t >= 7) {
        if (d[t - 1] == static_cast<std::uint8_t>('r') &&
            std::memcmp(d + t - 7, "trailer", 7) == 0) {
            const std::size_t kw = t - 7;
            const bool starts_buffer = (kw == 0);
            const bool clean_before =
                starts_buffer || is_ws(d[kw - 1]) ||
                (is_delimiter(d[kw - 1]) && d[kw - 1] != '/');
            if (clean_before) {
                found = true;
                t = kw;
                break;
            }
        }
        --t;
    }
    if (!found) {
        return;
    }

    // 3. Forward from just after `trailer`: whitespace, then the opening
    //    "<<", and the string-aware matching ">>" must land exactly on
    //    the close found in step 1. This re-verification makes decoy
    //    `trailer` text (inside a string, or in junk) harmless: a wrong
    //    candidate simply fails to line up and the dictionary fields
    //    stay empty instead of capturing wrong bytes.
    std::size_t q = t + 7;
    while (q < size && is_ws(d[q])) {
        ++q;
    }
    if (q + 1 >= size || d[q] != static_cast<std::uint8_t>('<') ||
        d[q + 1] != static_cast<std::uint8_t>('<')) {
        return;
    }
    const std::size_t open = q;        // first '<' of "<<"
    const std::size_t body = q + 2;    // first dictionary body byte
    const std::size_t match = matching_dict_close(d, open, size);
    if (match != close) {
        return;  // brackets do not line up with the pre-keyword ">": bail
    }

    info.dict = std::string_view(
        reinterpret_cast<const char*>(d + body), close - body);
    extract_root_and_prev(d, body, close, info);
}

}  // namespace

namespace detail {

void set_search_force_scalar(bool force) noexcept { g_force_scalar = force; }

}  // namespace detail

TrailerInfo locate_startxref(std::span<const std::uint8_t> bytes,
                             std::size_t tail_window) {
    if (bytes.empty()) {
        throw PdfToolkitException(ErrorCode::InvalidArgument,
                                  "locate_startxref: empty document");
    }
    if (tail_window == 0) {
        throw PdfToolkitException(ErrorCode::InvalidArgument,
                                  "locate_startxref: zero tail window");
    }

    const std::uint8_t* d = bytes.data();
    const std::size_t size = bytes.size();
    const std::size_t lo = size > tail_window ? size - tail_window : 0;

    TrailerInfo info;
    std::size_t limit = size;
    while (true) {
        const std::size_t p = highest_s_below(d, lo, limit);
        if (p == kNpos) {
            break;
        }
        limit = p;  // next candidate strictly below this one

        // Validate the keyword: nine exact bytes plus a terminator. A
        // keyword at the very buffer end has no terminator and is a
        // different token; a keyword glued to a regular character
        // ("startxrefX") is likewise not the keyword.
        if (p + 9 >= size) {
            continue;
        }
        if (std::memcmp(d + p, "startxref", 9) != 0) {
            continue;
        }
        if (!is_ws(d[p + 9])) {
            continue;
        }

        // Parse the offset (audit step 3: 64-bit integer).
        std::size_t q = p + 9;
        while (q < size && is_ws(d[q])) {
            ++q;
        }
        std::uint64_t offset = 0;
        if (!scan::parse_u64(d, q, size, offset)) {
            continue;  // tolerant: try the next candidate further back
        }

        info.xref_offset = offset;
        info.keyword_offset = p;
        extract_trailer(d, p, size, info);
        return info;
    }

    throw PdfToolkitException(
        ErrorCode::UnreadablePdf,
        "locate_startxref: no valid startxref in the scanned tail");
}

}  // namespace pdftoolkit::parser
