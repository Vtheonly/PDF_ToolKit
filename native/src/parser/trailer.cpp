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

constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

// PDF whitespace (ISO 32000 §7.2): NUL, TAB, LF, FF, CR, SP.
constexpr bool is_ws(std::uint8_t c) noexcept {
    return c == 0x00 || c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D ||
           c == 0x20;
}

// PDF delimiters (ISO 32000 §7.2).
constexpr bool is_delimiter(std::uint8_t c) noexcept {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' ||
           c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}

// A "regular" character: neither whitespace nor a delimiter — the only
// characters allowed inside keywords and names.
constexpr bool is_regular(std::uint8_t c) noexcept {
    return !is_ws(c) && !is_delimiter(c);
}

constexpr bool is_digit(std::uint8_t c) noexcept {
    return c >= '0' && c <= '9';
}

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
// Unsigned decimal parser. Returns true and sets `value` when [pos, end)
// starts with at least one digit, the number fits in uint64_t, and the
// number is terminated by whitespace, a delimiter or end-of-buffer (the
// PDF integer token rule — e.g. `123%%EOF` is the integer 123 followed
// by a comment). A leading '+' is tolerated (legal PDF integer syntax);
// a leading '-' is not (offsets are byte positions and cannot be
// negative). `pos` is advanced past the number on success only.
// ---------------------------------------------------------------------------
bool parse_u64(const std::uint8_t* d, std::size_t& pos, std::size_t end,
               std::uint64_t& value) noexcept {
    std::size_t q = pos;
    if (q < end && d[q] == static_cast<std::uint8_t>('+')) {
        ++q;
    }
    if (q >= end || !is_digit(d[q])) {
        return false;
    }
    std::uint64_t acc = 0;
    while (q < end && is_digit(d[q])) {
        const std::uint64_t digit =
            static_cast<std::uint64_t>(d[q] - static_cast<std::uint8_t>('0'));
        if (acc > (UINT64_MAX - digit) / 10) {
            return false;  // overflow: >20 digits cannot be a file offset
        }
        acc = acc * 10 + digit;
        ++q;
    }
    if (q < end && !is_ws(d[q]) && !is_delimiter(d[q])) {
        return false;  // "123x" — the token continues; not a clean integer
    }
    value = acc;
    pos = q;
    return true;
}

// Unsigned decimal parser clamped to uint32_t (object / generation
// numbers). Same terminator rules as parse_u64.
bool parse_u32(const std::uint8_t* d, std::size_t& pos, std::size_t end,
               std::uint32_t& value) noexcept {
    std::size_t q = pos;
    if (q < end && d[q] == static_cast<std::uint8_t>('+')) {
        ++q;
    }
    if (q >= end || !is_digit(d[q])) {
        return false;
    }
    std::uint64_t acc = 0;
    while (q < end && is_digit(d[q])) {
        const std::uint64_t digit =
            static_cast<std::uint64_t>(d[q] - static_cast<std::uint8_t>('0'));
        if (acc > (UINT64_MAX - digit) / 10) {
            return false;
        }
        acc = acc * 10 + digit;
        ++q;
    }
    if (q < end && !is_ws(d[q]) && !is_delimiter(d[q])) {
        return false;
    }
    if (acc > UINT32_MAX) {
        return false;
    }
    value = static_cast<std::uint32_t>(acc);
    pos = q;
    return true;
}

// ---------------------------------------------------------------------------
// String-aware forward scanning helpers (shared by the dictionary
// re-verification and the /Root & /Prev extraction).
// ---------------------------------------------------------------------------

// Skips a literal string starting at the '(' at `pos`. Returns the
// position just past the matching ')' (handles balanced parens and
// backslash escapes), or kNpos when the string is unterminated.
std::size_t skip_literal_string(const std::uint8_t* d, std::size_t pos,
                                std::size_t end) noexcept {
    std::size_t q = pos + 1;
    unsigned depth = 1;
    while (q < end && depth > 0) {
        if (d[q] == static_cast<std::uint8_t>('\\')) {
            q += 2;
            continue;
        }
        if (d[q] == static_cast<std::uint8_t>('(')) {
            ++depth;
        } else if (d[q] == static_cast<std::uint8_t>(')')) {
            --depth;
        }
        ++q;
    }
    return depth == 0 ? q : kNpos;
}

// Skips a hex string starting at the '<' at `pos` (a "<<" dict open must
// be consumed by the caller before this is reached). Returns the
// position just past the closing '>', or kNpos when unterminated.
std::size_t skip_hex_string(const std::uint8_t* d, std::size_t pos,
                            std::size_t end) noexcept {
    std::size_t q = pos + 1;
    while (q < end && d[q] != static_cast<std::uint8_t>('>')) {
        ++q;
    }
    return q < end ? q + 1 : kNpos;
}

// Given the position of the opening "<<" at `pos`, returns the position
// OF the first '>' of the matching ">>" — with literal/hex strings and
// nested dictionaries handled — or kNpos when unbalanced.
std::size_t matching_dict_close(const std::uint8_t* d, std::size_t pos,
                                std::size_t end) noexcept {
    std::size_t q = pos + 2;
    unsigned depth = 1;
    while (q < end) {
        const std::uint8_t c = d[q];
        if (c == static_cast<std::uint8_t>('(')) {
            q = skip_literal_string(d, q, end);
            if (q == kNpos) {
                return kNpos;
            }
        } else if (c == static_cast<std::uint8_t>('<')) {
            if (q + 1 < end && d[q + 1] == static_cast<std::uint8_t>('<')) {
                ++depth;
                q += 2;
            } else {
                q = skip_hex_string(d, q, end);
                if (q == kNpos) {
                    return kNpos;
                }
            }
        } else if (c == static_cast<std::uint8_t>('>')) {
            if (q + 1 < end && d[q + 1] == static_cast<std::uint8_t>('>')) {
                --depth;
                if (depth == 0) {
                    return q;
                }
                q += 2;
            } else {
                return kNpos;  // stray '>' outside any string: malformed
            }
        } else {
            ++q;
        }
    }
    return kNpos;
}

// ---------------------------------------------------------------------------
// Trailer dictionary extraction (best-effort; never produces wrong data).
// ---------------------------------------------------------------------------

// Walks the dictionary body [pos, end) (the brackets excluded), skipping
// strings, nested dictionaries and comments, and records /Root and /Prev
// at nesting depth 1 only. Decoys inside strings or nested dictionaries
// are invisible by construction.
void extract_root_and_prev(const std::uint8_t* d, std::size_t pos,
                           std::size_t end, TrailerInfo& info) noexcept {
    std::size_t q = pos;
    unsigned depth = 1;  // inside the dictionary whose body we walk
    while (q < end) {
        const std::uint8_t c = d[q];
        if (c == static_cast<std::uint8_t>('(')) {
            q = skip_literal_string(d, q, end);
            if (q == kNpos) {
                return;
            }
        } else if (c == static_cast<std::uint8_t>('<')) {
            if (q + 1 < end && d[q + 1] == static_cast<std::uint8_t>('<')) {
                ++depth;
                q += 2;
            } else {
                q = skip_hex_string(d, q, end);
                if (q == kNpos) {
                    return;
                }
            }
        } else if (c == static_cast<std::uint8_t>('>')) {
            if (q + 1 < end && d[q + 1] == static_cast<std::uint8_t>('>')) {
                --depth;
                if (depth == 0) {
                    return;  // body end reached
                }
                q += 2;
            } else {
                return;
            }
        } else if (c == static_cast<std::uint8_t>('%')) {
            while (q < end && d[q] != static_cast<std::uint8_t>('\n') &&
                   d[q] != static_cast<std::uint8_t>('\r')) {
                ++q;
            }
        } else if (c == static_cast<std::uint8_t>('/')) {
            const std::size_t name_start = ++q;
            while (q < end && is_regular(d[q])) {
                ++q;
            }
            const std::size_t name_len = q - name_start;
            if (depth != 1) {
                continue;  // name inside a nested dictionary: not ours
            }
            if (name_len == 4 &&
                std::memcmp(d + name_start, "Root", 4) == 0 && !info.has_root) {
                // `/Root <obj> <gen> R`, each token separated by at least
                // one whitespace (PDF token syntax).
                std::size_t r = q;
                while (r < end && is_ws(d[r])) {
                    ++r;
                }
                std::uint32_t obj = 0;
                std::uint32_t gen = 0;
                if (!parse_u32(d, r, end, obj)) {
                    continue;
                }
                const std::size_t after_obj = r;
                std::size_t s = r;
                while (s < end && is_ws(d[s])) {
                    ++s;
                }
                if (s == after_obj) {
                    continue;  // tokens must be whitespace-separated
                }
                if (!parse_u32(d, s, end, gen)) {
                    continue;
                }
                const std::size_t after_gen = s;
                std::size_t t = s;
                while (t < end && is_ws(d[t])) {
                    ++t;
                }
                if (t == after_gen || t >= end ||
                    d[t] != static_cast<std::uint8_t>('R') ||
                    (t + 1 < end && is_regular(d[t + 1]))) {
                    continue;  // must be the keyword R, terminated
                }
                info.has_root = true;
                info.root_object = obj;
                info.root_generation = gen;
                q = t + 1;
            } else if (name_len == 4 &&
                       std::memcmp(d + name_start, "Prev", 4) == 0 &&
                       !info.has_prev) {
                std::size_t r = q;
                while (r < end && is_ws(d[r])) {
                    ++r;
                }
                std::uint64_t prev = 0;
                if (!parse_u64(d, r, end, prev)) {
                    continue;
                }
                // `/Prev` must be a plain integer; an indirect reference
                // (`/Prev 4 0 R`, broken files) is recorded as absent.
                std::size_t s = r;
                while (s < end && is_ws(d[s])) {
                    ++s;
                }
                std::size_t probe = s;
                std::uint32_t dummy = 0;
                if (s < end && is_digit(d[s]) && parse_u32(d, probe, end, dummy)) {
                    std::size_t t = probe;
                    while (t < end && is_ws(d[t])) {
                        ++t;
                    }
                    if (t < end && d[t] == static_cast<std::uint8_t>('R') &&
                        (t + 1 >= end || !is_regular(d[t + 1]))) {
                        continue;  // indirect reference, not an offset
                    }
                }
                info.has_prev = true;
                info.prev_offset = prev;
                q = s;
            }
        } else {
            ++q;
        }
    }
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
        if (!parse_u64(d, q, size, offset)) {
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
