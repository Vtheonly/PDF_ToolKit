// Zero-copy byte lexer (audit issue-1/task-2.3).
//
// Emits PdfToken slices into the caller's buffer (zero-copy, zero
// allocations). Whitespace and comments are stripped through a
// 256-entry branchless lookup table — the audit's explicit
// prescription (`alignas(64) static const bool is_ws[256]`). The
// character classes mirror scan_util.hpp's (single source of truth for
// the classification RULES; the LUT materializes them for the hot
// loop). When task 2.3's successor phases formalize tokenization
// further, keep scan_util.hpp and this LUT synchronized.

#include "pdftoolkit/parser/lexer.hpp"

#include <charconv>

#include "scan_util.hpp"

namespace pdftoolkit::parser {
namespace {

// The audit's branchless whitespace table. uint8_t (0/1) rather than
// bool: zero-extends into a register without sign issues.
struct WhitespaceTable {
    std::uint8_t entry[256];
    constexpr WhitespaceTable() : entry{} {
        entry[0x00] = 1;  // NUL
        entry[0x09] = 1;  // TAB
        entry[0x0A] = 1;  // LF
        entry[0x0C] = 1;  // FF
        entry[0x0D] = 1;  // CR
        entry[0x20] = 1;  // SP
    }
};
alignas(64) constexpr WhitespaceTable kWs{};

// Skips whitespace and %-comments (both are token separators).
std::size_t skip_separators(const std::uint8_t* d, std::size_t pos,
                            std::size_t end) noexcept {
    while (pos < end) {
        if (kWs.entry[d[pos]]) {
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

// Reads a literal string starting at the '(' at `pos`: balanced parens
// and backslash escapes honored (the escape only guarantees the next
// byte cannot terminate the string). Returns the position just past the
// matching ')' — or `end` when unterminated (tolerant).
std::size_t scan_literal_string(const std::uint8_t* d, std::size_t pos,
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
    return depth == 0 ? q : end;
}

// Scans a hex string starting at the '<' at `pos` (the caller has
// already ruled out "<<"): to the first '>' or the buffer end.
std::size_t scan_hex_string(const std::uint8_t* d, std::size_t pos,
                            std::size_t end) noexcept {
    std::size_t q = pos + 1;
    while (q < end && d[q] != static_cast<std::uint8_t>('>')) {
        ++q;
    }
    return q < end ? q + 1 : end;
}

}  // namespace

ZeroCopyLexer::ZeroCopyLexer(std::span<const std::uint8_t> bytes) noexcept
    : d_(bytes.data()), size_(bytes.size()) {}

PdfToken ZeroCopyLexer::next() noexcept {
    for (;;) {  // iterative skip loop: hostile runs of skip bytes must
        // never recurse (a megabyte of '{' would overflow the stack)
        pos_ = skip_separators(d_, pos_, size_);
        if (pos_ >= size_) {
            return PdfToken{};  // EndOfFile, empty value
        }

        const std::uint8_t c = d_[pos_];
        const std::size_t start = pos_;

        auto make = [this](PdfToken::Type type, std::size_t from,
                           std::size_t to) noexcept {
            PdfToken t;
            t.type = type;
            t.value = std::string_view(
                reinterpret_cast<const char*>(d_ + from), to - from);
            pos_ = to;
            return t;
        };

        switch (c) {
            case static_cast<std::uint8_t>('('): {
                const std::size_t after =
                    scan_literal_string(d_, pos_, size_);
                return make(PdfToken::Type::StringLit, start, after);
            }
            case static_cast<std::uint8_t>('<'):
                if (pos_ + 1 < size_ &&
                    d_[pos_ + 1] == static_cast<std::uint8_t>('<')) {
                    return make(PdfToken::Type::DictStart, start, start + 2);
                }
                {
                    const std::size_t after =
                        scan_hex_string(d_, pos_, size_);
                    return make(PdfToken::Type::HexStr, start, after);
                }
            case static_cast<std::uint8_t>('>'):
                if (pos_ + 1 < size_ &&
                    d_[pos_ + 1] == static_cast<std::uint8_t>('>')) {
                    return make(PdfToken::Type::DictEnd, start, start + 2);
                }
                ++pos_;  // stray '>': tolerant skip (documented)
                continue;
            case static_cast<std::uint8_t>('['):
                return make(PdfToken::Type::ArrayStart, start, start + 1);
            case static_cast<std::uint8_t>(']'):
                return make(PdfToken::Type::ArrayEnd, start, start + 1);
            case static_cast<std::uint8_t>('{'):
            case static_cast<std::uint8_t>('}'):
                // Type-4 (PostScript calculator) braces: outside this
                // lexer's token set — tolerant skip (documented).
                ++pos_;
                continue;
            case static_cast<std::uint8_t>('/'): {
                const std::size_t name_end =
                    pos_ + 1 + scan::name_length(d_, pos_ + 1, size_);
                return make(PdfToken::Type::Name, start, name_end);
            }
            default: {
                // Number or keyword: a run of regular characters.
                const std::size_t run_end =
                    pos_ + scan::name_length(d_, pos_, size_);
                if (run_end == start) {
                    // Any other delimiter byte that is not in our token
                    // set cannot happen (all delimiters are handled
                    // above); defensive: skip one byte and continue.
                    ++pos_;
                    continue;
                }
                const bool numeric =
                    (c >= static_cast<std::uint8_t>('0') &&
                     c <= static_cast<std::uint8_t>('9')) ||
                    c == static_cast<std::uint8_t>('+') ||
                    c == static_cast<std::uint8_t>('-') ||
                    c == static_cast<std::uint8_t>('.');
                if (!numeric) {
                    const std::string_view text(
                        reinterpret_cast<const char*>(d_ + start),
                        run_end - start);
                    if (text == "stream") {
                        return make(PdfToken::Type::StreamStart, start,
                                    run_end);
                    }
                    if (text == "endstream") {
                        return make(PdfToken::Type::StreamEnd, start,
                                    run_end);
                    }
                    return make(PdfToken::Type::Keyword, start, run_end);
                }
                // A real if it contains '.', 'e' or 'E'; an integer
                // otherwise. The raw value is always preserved;
                // parsing is best-effort (from_chars is
                // locale-independent).
                bool real = false;
                for (std::size_t q = start; q < run_end; ++q) {
                    const std::uint8_t b = d_[q];
                    if (b == static_cast<std::uint8_t>('.') ||
                        b == static_cast<std::uint8_t>('e') ||
                        b == static_cast<std::uint8_t>('E')) {
                        real = true;
                        break;
                    }
                }
                PdfToken t =
                    real ? make(PdfToken::Type::Real, start, run_end)
                         : make(PdfToken::Type::Integer, start, run_end);
                const char* first =
                    reinterpret_cast<const char*>(d_ + start);
                const char* last = first + (run_end - start);
                if (real) {
                    const auto result =
                        std::from_chars(first, last, t.float_val);
                    (void)result;  // unparseable -> value stays 0
                } else {
                    // from_chars rejects a leading '+' for integers;
                    // PDF allows it — skip it before parsing.
                    if (first[0] == '+') {
                        ++first;
                    }
                    if (first < last) {
                        std::int64_t parsed = 0;
                        const auto result =
                            std::from_chars(first, last, parsed);
                        if (result.ec == std::errc{}) {
                            t.int_val = parsed;
                        } else if (result.ec ==
                                   std::errc::result_out_of_range) {
                            // Saturate (MuPDF-style tolerance).
                            t.int_val = (first[0] == '-') ? INT64_MIN
                                                          : INT64_MAX;
                        }
                    }
                }
                return t;
            }
        }
    }
}

}  // namespace pdftoolkit::parser
