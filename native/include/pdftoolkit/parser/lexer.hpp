#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace pdftoolkit::parser {

/// One lexical token over a PDF byte stream (audit task 2.3).
///
/// `value` is a ZERO-COPY slice into the lexer's buffer. Raw semantics
/// (documented deviations from a fully-decoding lexer — decoding would
/// need a copy and is deferred to consumers):
///   * StringLit spans the outer parentheses, escapes intact;
///   * HexStr spans the angle brackets, digits untouched;
///   * Name keeps `#`-hex escapes intact (`/A#20B` is raw "A#20B" —
///     the decoded name "A B" is the consumer's concern);
///   * numbers keep their sign/decimal text; `int_val`/`float_val`
///     carry the parsed value (saturating on overflow).
struct PdfToken {
    enum class Type : std::uint8_t {
        Keyword,
        Integer,
        Real,
        StringLit,
        HexStr,
        Name,
        DictStart,
        DictEnd,
        ArrayStart,
        ArrayEnd,
        StreamStart,
        StreamEnd,
        EndOfFile,  // audit enum extension: exhaustion must be
                    // expressible (see task-registry notes)
    };

    Type type = Type::EndOfFile;
    std::string_view value;
    std::int64_t int_val = 0;  // valid when type == Integer (saturating)
    float float_val = 0.0F;    // valid when type == Real
};

/// Zero-copy byte lexer over PDF content (audit task 2.3): emits
/// PdfToken slices pointing into the caller's buffer — no heap
/// allocation per token, no byte copies.
///
/// Contract:
///   * `next()` returns tokens in order and finally a token with
///     type == EndOfFile (whose `value` is empty; further `next()` calls
///     keep returning it);
///   * whitespace (incl. NUL) and `%` comments are skipped by a
///     256-entry branchless lookup table (the audit's prescription);
///   * `stream` / `endstream` arrive as StreamStart / StreamEnd — the
///     audit's enum calls them out as distinct types;
///   * after a StreamStart the CALLER owns positioning: stream payload
///     is binary and must not be tokenized. Use offset() (which points
///     just past the `stream` keyword) and skip the single EOL to find
///     the data start; the lexer itself never scans for `endstream`;
///   * tolerant by design (real-world corpora are hostile): stray
///     unpaired `<`/`>` and the type-4 braces `{`/`}` are skipped,
///     unterminated strings end at the buffer end, unparseable numbers
///     keep their raw value with zeroed numeric fields. The lexer never
///     throws and never reads out of bounds.
class ZeroCopyLexer {
public:
    explicit ZeroCopyLexer(std::span<const std::uint8_t> bytes) noexcept;

    /// Returns the next token (EndOfFile once exhausted; idempotent).
    [[nodiscard]] PdfToken next() noexcept;

    /// Bytes consumed so far (just past the last token's end; after a
    /// StreamStart token this points just past the keyword).
    [[nodiscard]] std::size_t offset() const noexcept { return pos_; }

    /// Total buffer size (for bounds arithmetic around stream payloads).
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    ZeroCopyLexer(const ZeroCopyLexer&) = delete;
    ZeroCopyLexer& operator=(const ZeroCopyLexer&) = delete;
    ZeroCopyLexer(ZeroCopyLexer&&) noexcept = default;
    ZeroCopyLexer& operator=(ZeroCopyLexer&&) noexcept = default;

private:
    const std::uint8_t* d_ = nullptr;
    std::size_t size_ = 0;
    std::size_t pos_ = 0;
};

}  // namespace pdftoolkit::parser
