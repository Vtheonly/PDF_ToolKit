// scan_util.hpp — shared low-level PDF scanning primitives.
//
// INTERNAL header (lives under src/, never installed, never included
// from public headers). Extracted from trailer.cpp (task 2.1) when
// task 2.2 needed the same character classes, string-skippers, number
// parsers and dictionary walker — extend, never fork (AGENTS.md §1.6).
//
// Task 2.3 (zero-copy lexer) will formalize tokenization as a public
// component; when it lands, reassess which of these primitives migrate
// into it. Until then this header stays the single implementation.

#ifndef PDFTOOLKIT_PARSER_SCAN_UTIL_HPP
#define PDFTOOLKIT_PARSER_SCAN_UTIL_HPP

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace pdftoolkit::parser::scan {

inline constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

// --- character classes (ISO 32000 §7.2) ------------------------------------

// PDF whitespace: NUL, TAB, LF, FF, CR, SP.
inline constexpr bool is_ws(std::uint8_t c) noexcept {
    return c == 0x00 || c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D ||
           c == 0x20;
}

// PDF delimiters: ( ) < > [ ] { } / %.
inline constexpr bool is_delimiter(std::uint8_t c) noexcept {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' ||
           c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}

// A "regular" character: neither whitespace nor delimiter — the only
// characters allowed inside keywords and names.
inline constexpr bool is_regular(std::uint8_t c) noexcept {
    return !is_ws(c) && !is_delimiter(c);
}

inline constexpr bool is_digit(std::uint8_t c) noexcept {
    return c >= '0' && c <= '9';
}

// --- forward skips -----------------------------------------------------------

// Whitespace (incl. NUL) forward.
inline std::size_t skip_ws(const std::uint8_t* d, std::size_t pos,
                           std::size_t end) noexcept {
    while (pos < end && is_ws(d[pos])) {
        ++pos;
    }
    return pos;
}

// Whitespace and comments (a '%' runs to the end of the line; comments
// are whitespace-equivalent per the PDF grammar) forward.
inline std::size_t skip_ws_and_comments(const std::uint8_t* d, std::size_t pos,
                                        std::size_t end) noexcept {
    while (pos < end) {
        if (is_ws(d[pos])) {
            ++pos;
        } else if (d[pos] == static_cast<std::uint8_t>('%')) {
            while (pos < end && d[pos] != static_cast<std::uint8_t>('\n') &&
                   d[pos] != static_cast<std::uint8_t>('\r')) {
                ++pos;
            }
        } else {
            break;
        }
    }
    return pos;
}

// --- string & dictionary skippers (zero-copy: positions only) -----------------

// Skips a literal string starting at the '(' at `pos`. Returns the
// position just past the matching ')' (handles balanced parens and
// backslash escapes), or kNpos when the string is unterminated.
inline std::size_t skip_literal_string(const std::uint8_t* d, std::size_t pos,
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

// Skips a hex string starting at the '<' at `pos` (a "<<" dict open is
// never passed here). Returns the position just past the closing '>',
// or kNpos when unterminated.
inline std::size_t skip_hex_string(const std::uint8_t* d, std::size_t pos,
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
inline std::size_t matching_dict_close(const std::uint8_t* d, std::size_t pos,
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

// --- numbers -------------------------------------------------------------------

// Unsigned decimal parser. Returns true and sets `value` when [pos, end)
// starts with at least one digit, the number fits in uint64_t, and the
// number is terminated by whitespace, a delimiter or end-of-buffer (the
// PDF integer token rule — e.g. `123%%EOF` is the integer 123 followed
// by a comment). A leading '+' is tolerated (legal PDF integer syntax);
// a leading '-' is not (offsets and object numbers cannot be negative).
// `pos` is advanced past the number on success only.
inline bool parse_u64(const std::uint8_t* d, std::size_t& pos, std::size_t end,
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
            return false;  // overflow: cannot be a file offset or object id
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

// Same contract as parse_u64, clamped to uint32_t (object and
// generation numbers).
inline bool parse_u32(const std::uint8_t* d, std::size_t& pos, std::size_t end,
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

// --- tokens --------------------------------------------------------------------

// True when the keyword `kw` sits exactly at `pos` and is terminated
// (followed by whitespace, a delimiter, or the end of the buffer — a
// keyword glued to a regular character is a different token).
inline bool keyword_at(const std::uint8_t* d, std::size_t pos,
                       std::size_t end, std::string_view kw) noexcept {
    if (pos > end || end - pos < kw.size()) {
        return false;
    }
    if (std::memcmp(d + pos, kw.data(), kw.size()) != 0) {
        return false;
    }
    const std::size_t after = pos + kw.size();
    return after >= end || !is_regular(d[after]);
}

// Length of the regular-character name starting at `pos` (the caller
// consumed the '/').
inline std::size_t name_length(const std::uint8_t* d, std::size_t pos,
                               std::size_t end) noexcept {
    std::size_t q = pos;
    while (q < end && is_regular(d[q])) {
        ++q;
    }
    return q - pos;
}

// Parses `[e1 e2 ...]` starting at `pos` (leading whitespace/comments
// allowed). Appends to `out`; returns false on malformed content.
// `pos` is advanced past the ']' on success.
inline bool parse_array_u64(const std::uint8_t* d, std::size_t& pos,
                            std::size_t end, std::vector<std::uint64_t>& out) {
    std::size_t q = skip_ws_and_comments(d, pos, end);
    if (q >= end || d[q] != static_cast<std::uint8_t>('[')) {
        return false;
    }
    ++q;
    while (true) {
        q = skip_ws_and_comments(d, q, end);
        if (q >= end) {
            return false;
        }
        if (d[q] == static_cast<std::uint8_t>(']')) {
            pos = q + 1;
            return true;
        }
        std::uint64_t v = 0;
        if (!parse_u64(d, q, end, v)) {
            return false;
        }
        out.push_back(v);
    }
}

// --- dictionary entry walker -----------------------------------------------------

// Skips one PDF value starting at `pos` (leading whitespace/comments
// allowed): a name, string, array, dictionary, number or keyword
// (including the multi-token `int int R` reference's first token —
// callers that need reference semantics parse the value region
// themselves). Arrays and dictionaries recurse through the same rules.
// Returns the position after the value, or kNpos when malformed.
inline std::size_t skip_value(const std::uint8_t* d, std::size_t pos,
                              std::size_t end) noexcept {
    pos = skip_ws_and_comments(d, pos, end);
    if (pos >= end) {
        return kNpos;
    }
    const std::uint8_t c = d[pos];
    if (c == static_cast<std::uint8_t>('/')) {
        return pos + 1 + name_length(d, pos + 1, end);  // a name
    }
    if (c == static_cast<std::uint8_t>('(')) {
        return skip_literal_string(d, pos, end);
    }
    if (c == static_cast<std::uint8_t>('<')) {
        if (pos + 1 < end && d[pos + 1] == static_cast<std::uint8_t>('<')) {
            const std::size_t close = matching_dict_close(d, pos, end);
            return close == kNpos ? kNpos : close + 2;
        }
        return skip_hex_string(d, pos, end);
    }
    if (c == static_cast<std::uint8_t>('[')) {
        std::size_t q = pos + 1;
        while (true) {
            q = skip_ws_and_comments(d, q, end);
            if (q >= end) {
                return kNpos;
            }
            if (d[q] == static_cast<std::uint8_t>(']')) {
                return q + 1;
            }
            q = skip_value(d, q, end);
            if (q == kNpos) {
                return kNpos;
            }
        }
    }
    // Numbers and keywords (R, true, false, null): runs of regular
    // characters.
    return pos + name_length(d, pos, end);
}

// Walks a dictionary BODY (strictly between its `<<` and `>>`) and
// invokes cb(name, value_pos, value_end) for every `Name value` pair at
// nesting depth 1. A value region spans from after the name (leading
// whitespace/comments skipped) up to the next depth-1 name or the
// closing ">>"; consumers parse from the front and may ignore trailing
// bytes. After reading a key, the walker skips the value SYNTACTICALLY
// (skip_value) — a name value such as `/Type /XRef` must not be
// mistaken for the next key, and names inside arrays or nested
// dictionaries are invisible by construction. Malformed content simply
// stops the walk (entries found so far are still emitted).
template <typename F>
void for_each_dict_entry(const std::uint8_t* d, std::size_t pos,
                         std::size_t end, F&& cb) {
    std::size_t q = pos;
    unsigned depth = 1;
    std::string_view key;
    std::size_t value_start = kNpos;
    const auto emit = [&](std::size_t value_end) {
        if (value_start != kNpos && value_end != kNpos) {
            cb(key, value_start, value_end);
        }
        value_start = kNpos;
    };
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
                    emit(q);
                    return;
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
            const std::size_t name_pos = q + 1;
            const std::size_t len = name_length(d, name_pos, end);
            if (depth == 1) {
                emit(q);  // the previous value ends at this key's '/'
                key = std::string_view(
                    reinterpret_cast<const char*>(d + name_pos), len);
                value_start = skip_ws_and_comments(d, name_pos + len, end);
                // Consume the whole value syntactically so its leading
                // name slash (e.g. `/Type /XRef`) and any names inside
                // arrays or nested dictionaries cannot be mistaken for
                // the next key.
                const std::size_t after_value =
                    skip_value(d, name_pos + len, end);
                q = (after_value == kNpos) ? name_pos + len : after_value;
            } else {
                q = name_pos + len;
            }
        } else {
            ++q;
        }
    }
    // The body range ends here (the closing ">>" is OUTSIDE a body-only
    // range by contract). The pending value extends to `end`; without
    // this emit, the LAST entry of every dictionary would be dropped —
    // caught by the task-2.1 regression suite during the task-2.2
    // refactor (the walker originally emitted only on the next key or
    // the ">>", which a body-only range never contains).
    emit(end);
}

}  // namespace pdftoolkit::parser::scan

#endif  // PDFTOOLKIT_PARSER_SCAN_UTIL_HPP
