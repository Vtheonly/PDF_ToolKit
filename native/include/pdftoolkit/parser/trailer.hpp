#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "pdftoolkit/errors.hpp"

namespace pdftoolkit::parser {

/// Result of the backward `startxref` / trailer scan (audit task 2.1).
///
/// Everything is a view into, or an offset within, the caller's buffer —
/// the scan is zero-copy by construction (AGENTS.md §5, audit Layer 1).
struct TrailerInfo {
    /// Value of the located `startxref` offset: the byte position (from
    /// the document start) of the cross-reference section it names.
    std::uint64_t xref_offset = 0;
    /// Buffer position of the `s` of the located `startxref` keyword
    /// (diagnostics; also the anchor for the trailer-dictionary scan).
    std::uint64_t keyword_offset = 0;
    /// Zero-copy view of the classic trailer dictionary BODY — strictly
    /// between `<<` and its matching `>>`, brackets excluded. Empty when
    /// the document has no classic `trailer` section (XRef-stream files
    /// carry their dictionary on the stream object — parsed by task
    /// 2.2's resolver, not here) or when it is malformed.
    std::string_view dict;
    /// `/Root <obj> <gen> R` — the document catalog reference the audit
    /// asks task 2.1 to extract. `has_root == false` records absence or
    /// a malformed reference honestly (the caller decides whether that
    /// is fatal; XRef-stream files legitimately have no classic /Root).
    bool has_root = false;
    std::uint32_t root_object = 0;
    std::uint32_t root_generation = 0;
    /// `/Prev <offset>` — previous cross-reference section (incremental
    /// updates). Absent in single-section files.
    bool has_prev = false;
    std::uint64_t prev_offset = 0;
};

/// Scans the tail of a PDF buffer backward for the last valid
/// `startxref` keyword, parses its offset, and extracts the classic
/// trailer dictionary's `/Root` (and `/Prev`) entries — audit task 2.1.
///
/// Contract:
///   * `bytes` is the whole document (typically `MmapHandle::bytes()`);
///     only the last `tail_window` bytes (default 1024, the audit's
///     window) are searched for the keyword. Files whose trailing junk
///     pushes `startxref` further back are not invalid — callers pass a
///     larger `tail_window` (the audit's fixed 1024 is a tunable, not a
///     format limit; see the task-registry notes).
///   * candidates are validated in descending position order and the
///     FIRST candidate whose keyword is whitespace-terminated AND whose
///     offset parses as a non-negative 64-bit integer (optional `+`,
///     terminated by whitespace or end-of-buffer) wins — i.e. the last
///     valid `startxref` wins, matching every production PDF reader.
///     Near-miss candidates (`startxref` glued to a non-whitespace byte,
///     non-numeric or overflowing offsets) are skipped, not fatal.
///   * trailer-dictionary extraction is best-effort: the dictionary is
///     located by scanning backward from the keyword, then re-verified
///     forward with string-aware tokenisation (literal/hex strings and
///     nested dictionaries cannot plant decoy `/Root` entries). A
///     missing or unverifiable dictionary leaves `dict` empty and
///     `has_root`/`has_prev` false — never wrong data.
///   * the SIMD needle search (`_mm256_cmpeq_epi8` against `s`, per the
///     audit) is runtime-dispatched on x86-64 GCC/Clang builds; every
///     other platform (and MSVC) uses the equivalent portable scalar
///     backward scan. Both paths are behavioural equivalents locked by
///     the same tests.
///
/// Throws PdfToolkitException:
///   empty `bytes` or zero `tail_window` -> InvalidArgument
///   no valid `startxref` candidate      -> UnreadablePdf
[[nodiscard]] TrailerInfo locate_startxref(
    std::span<const std::uint8_t> bytes,
    std::size_t tail_window = 1024);

namespace detail {

/// Test hook (native/tests/test_trailer.cpp): force the portable scalar
/// backward search so the SIMD and scalar paths can be differential-
/// tested. NOT for production use; not thread-safe; tests must restore
/// it (false) before returning.
void set_search_force_scalar(bool force) noexcept;

}  // namespace detail

}  // namespace pdftoolkit::parser
