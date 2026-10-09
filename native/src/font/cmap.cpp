// Immutable global CMap cache & Unicode resolver (audit issue-1/task-3.1).
//
// Translates PDF font character codes to true UTF-32 codepoints by
// parsing /ToUnicode CMap programs:
//
//   * tokenization rides the task-2.3 ZeroCopyLexer (the dependency
//     the audit names) — beginbfchar/beginbfrange/begincodespacerange
//     blocks are keyword-delimited token regions, everything else
//     (PostScript preamble, defineresource epilogue, dicts, comments)
//     is skipped tolerantly, exactly like tasks 2.1-2.3 taught;
//   * destinations are UTF-16BE hex strings: surrogate pairs decode to
//     astral codepoints; presentation-form ligatures are expanded at
//     BUILD time (verified table below — the audit's 0xFB01 -> {f,i}
//     plus every other multi-character ligature in U+FB00..U+FB4F);
//   * the frozen table is a direct index over the mapped codespace
//     (O(1) lookup, one bounds check — the task-3.3 per-glyph hot
//     path), sized to the highest mapped code, capped at 2-byte scope
//     by entry-by-entry refusal (a hostile CMap cannot size it);
//   * the cache (task 3.1 step 4) deduplicates tables across the
//     engine instance by xxHash64 over the stream payload as it
//     appears in the file (in-repo xxhash64.hpp, ADR-0008), with a
//     byte-exact confirmation on every hash hit — hash collisions
//     cannot merge different CMaps.
//
// The stream-object anatomy (header / dictionary / payload location /
// filter classification / predictor detection / indirect-/Length
// refusal) is the shared stream_util.hpp extracted from xref.cpp for
// exactly this task — extend, never fork.

#include "pdftoolkit/font/cmap.hpp"

#include <algorithm>
#include <cstring>

#include "pdftoolkit/parser/lexer.hpp"
#include "../core/xxhash64.hpp"
#include "../parser/stream_util.hpp"

namespace pdftoolkit::font {
namespace {

using parser::PdfToken;
using parser::ZeroCopyLexer;

// ---------------------------------------------------------------------------
// Ligature table (audit task 3.1 step 3)
// ---------------------------------------------------------------------------

// Every codepoint in U+FB00..U+FB4F whose Unicode name is a LIGATURE
// with a multi-character decomposition — Latin (ff fi fl ffi ffl ſt
// st), Armenian (men-*, vew-now), Hebrew (yiddish double-yod patah,
// alef-lamed). Table verified against Python's unicodedata (see the
// task-3.1 evidence in the task registry); decompositions are the
// canonical Unicode ones (note U+FB05 -> LONG S + t, U+017F, not 's').
// The Hebrew base+diacritic compositions in the same block (FB1D,
// FB2A..FB4E — "LETTER X WITH Y") are deliberately NOT here: they are
// accent compositions, not ligatures, and expanding them would be
// Unicode normalization, not ligature repair (scope line recorded in
// the problem registry). Arabic Presentation Forms (U+FB50+) are
// excluded for the same reason plus shaping semantics.
struct Ligature {
    char32_t code;
    const char32_t* expansion;
    std::size_t length;
};

constexpr char32_t kLigFf[] = {0x0066, 0x0066};
constexpr char32_t kLigFi[] = {0x0066, 0x0069};
constexpr char32_t kLigFl[] = {0x0066, 0x006C};
constexpr char32_t kLigFfi[] = {0x0066, 0x0066, 0x0069};
constexpr char32_t kLigFfl[] = {0x0066, 0x0066, 0x006C};
constexpr char32_t kLigLongSt[] = {0x017F, 0x0074};
constexpr char32_t kLigSt[] = {0x0073, 0x0074};
constexpr char32_t kLigMenNow[] = {0x0574, 0x0576};
constexpr char32_t kLigMenEch[] = {0x0574, 0x0565};
constexpr char32_t kLigMenXeh[] = {0x0574, 0x056B};
constexpr char32_t kLigVewNow[] = {0x057E, 0x0576};
constexpr char32_t kLigMenIni[] = {0x0574, 0x056D};
constexpr char32_t kLigYiddishYod[] = {0x05F2, 0x05B7};
constexpr char32_t kLigAlefLamed[] = {0x05D0, 0x05DC};

// Sorted by code for binary search.
constexpr Ligature kLigatures[] = {
    {0xFB00, kLigFf, 2},         {0xFB01, kLigFi, 2},
    {0xFB02, kLigFl, 2},         {0xFB03, kLigFfi, 3},
    {0xFB04, kLigFfl, 3},        {0xFB05, kLigLongSt, 2},
    {0xFB06, kLigSt, 2},         {0xFB13, kLigMenNow, 2},
    {0xFB14, kLigMenEch, 2},     {0xFB15, kLigMenXeh, 2},
    {0xFB16, kLigVewNow, 2},     {0xFB17, kLigMenIni, 2},
    {0xFB1F, kLigYiddishYod, 2}, {0xFB4F, kLigAlefLamed, 2},
};

// Appends `cp` to `out`, expanded when it is a known ligature.
void append_expanded(std::vector<char32_t>& out, char32_t cp) {
    const auto* begin = std::begin(kLigatures);
    const auto* end = std::end(kLigatures);
    const auto* it = std::lower_bound(
        begin, end, cp,
        [](const Ligature& l, char32_t value) { return l.code < value; });
    if (it != end && it->code == cp) {
        out.insert(out.end(), it->expansion, it->expansion + it->length);
    } else {
        out.push_back(cp);
    }
}

// ---------------------------------------------------------------------------
// Hex-string primitives
// ---------------------------------------------------------------------------

// Hex digit value, or -1.
int hex_nibble(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Strips the angle brackets from a raw HexStr token value ("<0020>" ->
// "0020"); the lexer guarantees the leading '<' (or an unterminated
// run, which strips to whatever is there).
std::string_view hex_body(std::string_view raw) noexcept {
    if (!raw.empty() && raw.front() == '<') {
        if (raw.size() >= 2 && raw.back() == '>') {
            return raw.substr(1, raw.size() - 2);
        }
        return raw.substr(1);
    }
    return raw;
}

// Collects the hex digits of a hex-string body into CALLER-OWNED
// scratch (no per-token allocation — the parse path is allocation-
// free modulo the growth of the record/pool vectors). Whitespace is
// legal inside PDF hex strings and ignored; an odd trailing digit is
// padded with 0 per the PDF token rule. Returns false when a non-hex,
// non-whitespace byte is present (malformed).
bool collect_hex_digits(std::string_view raw,
                        std::vector<int>& digits) {
    digits.clear();
    for (const char c : hex_body(raw)) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
            c == '\0') {
            continue;
        }
        const int v = hex_nibble(c);
        if (v < 0) {
            return false;
        }
        digits.push_back(v);
    }
    if (digits.size() % 2 != 0) {
        digits.push_back(0);  // PDF padding rule
    }
    return true;
}

// Parses a hex string as a single big-endian VALUE (a source code).
// Returns false when empty or wider than 32 bits. (Not noexcept:
// scratch growth may throw std::bad_alloc, which CMapTable::parse's
// contract propagates.)
bool parse_hex_value(std::string_view raw, std::vector<int>& scratch,
                     std::uint32_t& value) {
    if (!collect_hex_digits(raw, scratch) || scratch.empty() ||
        scratch.size() > 8) {
        return false;
    }
    std::uint32_t v = 0;
    for (const int d : scratch) {
        v = (v << 4) | static_cast<std::uint32_t>(d);
    }
    value = v;
    return true;
}

// Width in BYTES of a hex string's value (codespace entries): the
// padded digit count / 2 (1-4 by the 32-bit cap). Returns false when
// malformed/empty.
bool parse_hex_width(std::string_view raw, std::vector<int>& scratch,
                     unsigned& width) {
    if (!collect_hex_digits(raw, scratch) || scratch.empty() ||
        scratch.size() > 8) {
        return false;
    }
    width = static_cast<unsigned>(scratch.size() / 2);
    return true;
}

// Defensive cap on one destination sequence AFTER ligature expansion.
// Real ToUnicode targets are 1-4 codepoints (ligatures reach 3); a
// "destination" beyond 64 is hostile junk sizing the pool — refused
// entry-by-entry like every other malformed value.
constexpr std::size_t kMaxDestinationCp = 64;

// Decodes a UTF-16BE hex-string destination into a ligature-EXPANDED
// codepoint sequence (build-time normalization: lookups return final
// text). One UTF-16 code unit = 4 hex digits; surrogate pairs combine
// into one astral codepoint. Returns false on invalid UTF-16 (lone
// surrogates), a non-hex body, an empty result, or a sequence past
// the defensive cap.
bool decode_destination(std::string_view raw,
                        std::vector<int>& scratch,
                        std::vector<char32_t>& out) {
    out.clear();
    if (!collect_hex_digits(raw, scratch)) {
        return false;
    }
    const std::vector<int>& digits = scratch;
    for (std::size_t i = 0; i + 3 < digits.size(); i += 4) {
        const std::uint32_t unit =
            (static_cast<std::uint32_t>(digits[i]) << 12) |
            (static_cast<std::uint32_t>(digits[i + 1]) << 8) |
            (static_cast<std::uint32_t>(digits[i + 2]) << 4) |
            static_cast<std::uint32_t>(digits[i + 3]);
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            // High surrogate: the NEXT unit must complete the pair.
            if (i + 7 >= digits.size()) {
                return false;
            }
            const std::uint32_t next =
                (static_cast<std::uint32_t>(digits[i + 4]) << 12) |
                (static_cast<std::uint32_t>(digits[i + 5]) << 8) |
                (static_cast<std::uint32_t>(digits[i + 6]) << 4) |
                static_cast<std::uint32_t>(digits[i + 7]);
            if (next < 0xDC00 || next > 0xDFFF) {
                return false;
            }
            const char32_t cp = static_cast<char32_t>(
                0x10000 + ((unit - 0xD800) << 10) + (next - 0xDC00));
            append_expanded(out, cp);
            i += 4;  // consumed the low surrogate's digits as well
            continue;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return false;  // lone low surrogate
        }
        append_expanded(out, static_cast<char32_t>(unit));
        if (out.size() > kMaxDestinationCp) {
            return false;
        }
    }
    return !out.empty();
}

// Decodes a hex string as EXACTLY ONE UTF-16 code unit's worth — the
// 3-operand bfrange base value (4 hex digits; a surrogate pair here
// is rejected: bases are arithmetic, and pair arithmetic is not
// defined). Expansion happens per GENERATED value later, not here.
bool decode_single_codepoint(std::string_view raw,
                             std::vector<int>& scratch,
                             char32_t& out) {
    if (!collect_hex_digits(raw, scratch) || scratch.size() != 4) {
        return false;
    }
    const std::uint32_t unit =
        (static_cast<std::uint32_t>(scratch[0]) << 12) |
        (static_cast<std::uint32_t>(scratch[1]) << 8) |
        (static_cast<std::uint32_t>(scratch[2]) << 4) |
        static_cast<std::uint32_t>(scratch[3]);
    if (unit >= 0xD800 && unit <= 0xDFFF) {
        return false;  // surrogates are not codepoints
    }
    out = static_cast<char32_t>(unit);
    return true;
}

// ---------------------------------------------------------------------------
// Token-driven block parser
// ---------------------------------------------------------------------------

// One mapping record while parsing; finalized by stable sort + last
// wins (PDF definition order overwrites).
struct Record {
    std::uint32_t code;
    std::uint32_t pool_offset;
    std::uint32_t pool_length;
};

// Scratch arena bytes for the cache's Flate staging. ToUnicode CMaps
// are KB-scale; 8 MiB covers a 128x-expanded 64 KiB stream (the codec
// defaults cap expansion there anyway).
constexpr std::size_t kFlateScratchBytes = std::size_t{8} << 20;

// xxHash64 domain-separation seeds: a raw CMap text and a Flate blob
// with identical bytes must never share a dedup key.
constexpr std::uint64_t kSeedRawPayload = 0x524157ULL;    // "RAW"
constexpr std::uint64_t kSeedFlatePayload = 0x464C54ULL;  // "FLT"

// Parser scratch over the token stream. Mode transitions happen only
// on the begin*/end* keywords; hex strings are meaningful only inside
// a block (preamble dicts cannot inject mappings by construction —
// their contents arrive as name/string/number tokens and are skipped).
enum class Mode {
    Idle,
    Codespace,
    Bfchar,
    Bfrange,
    BfrangeArray,
};

struct BlockParser {
    std::vector<Record>& recs;
    std::vector<char32_t>& pool;
    CMapStats& stats;

    std::vector<std::uint32_t> pending;  // bfrange lo/hi collection
    std::vector<char32_t> dst;           // decoded destination scratch
    std::vector<int> scratch_digits;     // hex-decoding scratch (shared;
                                         // keeps the parse path free of
                                         // per-token allocations)
    bool dead_src = false;               // bfchar alignment: a malformed
                                         // source consumes its destination
                                         // too (positional pair grammar)
    unsigned dead_hex = 0;               // bfrange alignment: a malformed
                                         // lo/hi kills the REST of the
                                         // triple (positional grammar)

    explicit BlockParser(std::vector<Record>& r, std::vector<char32_t>& p,
                         CMapStats& s)
        : recs(r), pool(p), stats(s) {}

    // Emits one mapping (code -> sequence). Returns false when the
    // code is out of the audit's 1-2-byte scope (counted skipped).
    bool emit(std::uint32_t code) {
        if (code > 0xFFFF) {
            ++stats.skipped_entries;
            return false;
        }
        recs.push_back(Record{code, static_cast<std::uint32_t>(pool.size()),
                              static_cast<std::uint32_t>(dst.size())});
        pool.insert(pool.end(), dst.begin(), dst.end());
        return true;
    }

    void run(const std::uint8_t* text, std::size_t size) {
        ZeroCopyLexer lexer(std::span<const std::uint8_t>(text, size));

        Mode mode = Mode::Idle;

        for (PdfToken t = lexer.next();; t = lexer.next()) {
            if (t.type == PdfToken::Type::EndOfFile) {
                break;  // unterminated block: entries so far are kept
            }

            switch (t.type) {
                case PdfToken::Type::Keyword: {
                    const std::string_view kw = t.value;
                    if (kw == "begincodespacerange") {
                        mode = Mode::Codespace;
                        pending.clear();
                        dead_src = false;
                        stats.saw_codespace = true;
                    } else if (kw == "endcodespacerange") {
                        mode = Mode::Idle;
                        pending.clear();
                        dead_src = false;
                    } else if (kw == "beginbfchar") {
                        mode = Mode::Bfchar;
                        pending.clear();
                        dead_src = false;
                        stats.saw_bf_blocks = true;
                    } else if (kw == "endbfchar") {
                        if (!pending.empty()) {
                            ++stats.skipped_entries;  // dangling source
                        }
                        mode = Mode::Idle;
                        pending.clear();
                        dead_src = false;
                    } else if (kw == "beginbfrange") {
                        mode = Mode::Bfrange;
                        pending.clear();
                        dead_src = false;
                        dead_hex = 0;
                        stats.saw_bf_blocks = true;
                    } else if (kw == "endbfrange") {
                        if (mode == Mode::Bfrange && !pending.empty()) {
                            ++stats.skipped_entries;  // dangling lo/hi
                        }
                        mode = Mode::Idle;
                        pending.clear();
                        dead_src = false;
                        dead_hex = 0;
                    }
                    // Other keywords (def, usecmap, findresource, ...)
                    // are preamble/epilogue noise — tolerated in place.
                    break;
                }

                case PdfToken::Type::HexStr: {
                    handle_hex(t.value, mode);
                    break;
                }
                case PdfToken::Type::ArrayStart: {
                    if (mode == Mode::Bfrange) {
                        // <lo> <hi> [ d1 d2 ... ] — cursor starts at lo.
                        // A malformed lo/hi pair absorbs and drops the
                        // array element-by-element (each counted).
                        mode = Mode::BfrangeArray;
                        dead_hex = 0;  // entry boundary: a dead lo/hi
                                       // dies via the absorb path below
                        if (pending.size() == 2 && pending[0] <= pending[1] &&
                            pending[1] <= 0xFFFF) {
                            const std::uint32_t lo = pending[0];
                            const std::uint32_t hi = pending[1];
                            pending.clear();
                            pending.push_back(lo);  // cursor
                            pending.push_back(hi);  // bound
                        } else {
                            ++stats.skipped_entries;
                            pending.clear();
                            pending.push_back(2);  // cursor > bound:
                            pending.push_back(1);  // every element skips
                        }
                    }
                    // Arrays elsewhere (preamble) are skipped.
                    break;
                }

                case PdfToken::Type::ArrayEnd: {
                    if (mode == Mode::BfrangeArray) {
                        mode = Mode::Bfrange;
                        pending.clear();
                        dead_hex = 0;
                    }
                    break;
                }

                default:
                    // Numbers (block counts — advisory; the terminators
                    // rule), names, strings, dict brackets: tolerated.
                    break;
            }
        }
    }

    void handle_hex(std::string_view raw, Mode mode) {
        switch (mode) {
            case Mode::Idle:
                break;  // hex inside dicts/preamble: ignored

            case Mode::Codespace: {
                // Pairs declare the code width. Adobe emits one pair;
                // for multi-pair (mixed-width) codespaces the WIDEST
                // declaration wins — a narrower split would truncate
                // codes the stream actually uses.
                std::uint32_t value = 0;
                unsigned width = 0;
                if (parse_hex_value(raw, scratch_digits, value) &&
                    parse_hex_width(raw, scratch_digits, width)) {
                    if (width > stats.code_bytes) {
                        stats.code_bytes = width;
                    }
                }
                break;
            }

            case Mode::Bfchar: {
                if (dead_src) {
                    // The previous source was malformed; this hex is
                    // its positional destination — the pair dies whole
                    // (keeps subsequent entries aligned).
                    dead_src = false;
                    ++stats.skipped_entries;
                    break;
                }
                if (!pending.empty()) {
                    // DST position: any malformed destination (empty,
                    // lone surrogates, non-hex) fails decode and the
                    // PAIR dies — never re-pairs a stray dst with the
                    // next entry's source. An out-of-scope SOURCE is
                    // counted skipped inside emit().
                    if (decode_destination(raw, scratch_digits, dst) &&
                        emit(pending.front())) {
                        ++stats.bfchar_entries;
                    } else if (dst.empty()) {
                        ++stats.skipped_entries;
                    }
                    pending.clear();
                    break;
                }
                std::uint32_t value = 0;
                if (!parse_hex_value(raw, scratch_digits, value)) {
                    ++stats.skipped_entries;
                    dead_src = true;
                    break;
                }
                pending.push_back(value);  // the source
                break;
            }

            case Mode::Bfrange: {
                if (dead_hex > 0) {
                    // A malformed lo/hi killed this triple; the rest
                    // of its positional slots die with it.
                    ++stats.skipped_entries;
                    --dead_hex;
                    break;
                }
                std::uint32_t value = 0;
                if (!parse_hex_value(raw, scratch_digits, value)) {
                    ++stats.skipped_entries;
                    // lo bad -> hi and dst die; hi bad -> dst dies.
                    dead_hex = pending.size() >= 2 ? 0
                                                   : (2 - static_cast<unsigned>(pending.size()));
                    pending.clear();
                    break;
                }
                if (pending.size() < 2) {
                    pending.push_back(value);  // lo, then hi
                } else {
                    // Third hex string: the 3-operand base value.
                    emit_three_operand(pending[0], pending[1], raw);
                    pending.clear();
                }
                break;
            }

            case Mode::BfrangeArray: {
                // Array elements map to cursor, cursor+1, ... while
                // the cursor stays inside [lo, hi].
                const bool in_range = pending[0] <= pending[1];
                if (decode_destination(raw, scratch_digits, dst) && in_range &&
                    emit(pending[0])) {
                    ++stats.bfrange_entries;
                } else {
                    ++stats.skipped_entries;
                }
                if (pending[0] < 0xFFFFFFFFU) {
                    ++pending[0];  // advance the cursor
                }
                break;
            }
        }
    }

    void emit_three_operand(std::uint32_t lo, std::uint32_t hi,
                            std::string_view raw_base) {
        char32_t base = 0;
        if (lo > hi || hi > 0xFFFF || !decode_single_codepoint(raw_base, scratch_digits, base)) {
            ++stats.skipped_entries;
            return;
        }
        const std::uint64_t span =
            static_cast<std::uint64_t>(hi) - lo;
        if (static_cast<std::uint64_t>(base) + span > 0x10FFFF) {
            ++stats.skipped_entries;  // would generate past Unicode
            return;
        }
        for (std::uint64_t k = 0; k <= span; ++k) {
            dst.clear();
            append_expanded(dst,
                            static_cast<char32_t>(base + k));
            if (emit(static_cast<std::uint32_t>(lo + k))) {
                ++stats.bfrange_entries;
            }
        }
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// CMapTable
// ---------------------------------------------------------------------------

std::shared_ptr<const CMapTable> CMapTable::parse(
    std::span<const std::uint8_t> cmap_text, CMapStats* stats_out) {
    auto table = std::make_shared<CMapTable>();
    CMapStats stats;

    std::vector<Record> recs;
    std::vector<char32_t> pool;
    BlockParser parser(recs, pool, stats);
    parser.run(cmap_text.data(), cmap_text.size());

    // Last-wins: stable sort keeps insertion order inside equal
    // codes; the last of each run is the surviving definition.
    std::stable_sort(recs.begin(), recs.end(),
                     [](const Record& a, const Record& b) {
                         return a.code < b.code;
                     });

    std::uint32_t max_code = 0;
    bool any = false;
    for (const Record& r : recs) {
        if (!any || r.code > max_code) {
            max_code = r.code;
            any = true;
        }
    }

    if (any) {
        table->index_.assign(static_cast<std::size_t>(max_code) + 1, 0);
        table->entries_.reserve(recs.size());
        const Record* winner = nullptr;
        const auto flush = [&]() {
            table->entries_.push_back(
                Entry{winner->pool_offset, winner->pool_length});
            table->index_[winner->code] =
                static_cast<std::uint32_t>(table->entries_.size());
            ++stats.mapped_codes;
        };
        for (const Record& r : recs) {
            if (winner != nullptr && winner->code != r.code) {
                flush();
            }
            winner = &r;  // later definitions overwrite earlier ones
        }
        if (winner != nullptr) {
            flush();
        }
        table->pool_ = std::move(pool);

        // Code width: declared codespace (widest declaration), else
        // inferred from the widest mapped source; 0 only when the CMap
        // maps nothing and declares nothing.
        if (stats.code_bytes == 0) {
            stats.code_bytes = max_code > 0xFF ? 2 : 1;
        }
    }

    table->mapped_codes_ = stats.mapped_codes;
    table->code_bytes_ = stats.code_bytes;
    if (stats_out != nullptr) {
        *stats_out = stats;
    }
    return table;
}

std::span<const char32_t> CMapTable::lookup(
    std::uint32_t code) const noexcept {
    if (code >= index_.size()) {
        return {};
    }
    const std::uint32_t entry_id = index_[code];
    if (entry_id == 0) {
        return {};
    }
    const Entry& e = entries_[entry_id - 1];
    return std::span<const char32_t>(pool_.data() + e.offset, e.length);
}

// ---------------------------------------------------------------------------
// CMapCache
// ---------------------------------------------------------------------------

std::size_t CMapCache::size() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

std::size_t CMapCache::intern_hits() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return intern_hits_;
}

// Finds the first `endstream` marker at/after `from` (raw payloads are
// text; the CMap grammar ends at endcmap long before). Returns the
// marker position, or `size` when absent.
std::size_t find_endstream(const std::uint8_t* d, std::size_t from,
                           std::size_t size) noexcept {
    if (size < 9 || from > size - 9) {
        return size;
    }
    for (std::size_t i = from; i + 9 <= size; ++i) {
        if (std::memcmp(d + i, "endstream", 9) == 0) {
            return i;
        }
    }
    return size;
}

std::shared_ptr<const CMapTable> CMapCache::intern_stream_object(
    std::span<const std::uint8_t> object_bytes, CMapInternReport* report) {
    CMapInternReport local;
    const std::uint8_t* d = object_bytes.data();
    const std::size_t size = object_bytes.size();

    const std::lock_guard<std::mutex> lock(mutex_);

    // Anatomy: `N G obj << dict >> stream <EOL> payload endstream`.
    std::size_t q = parser::stream::parse_indirect_header(d, size, 0);
    if (q == parser::scan::kNpos) {
        local.status = CMapStatus::BadObject;
        local.message = "not an indirect stream object";
        if (report != nullptr) {
            *report = local;
        }
        return nullptr;
    }
    q = parser::scan::skip_ws_and_comments(d, q, size);
    if (q + 1 >= size || d[q] != static_cast<std::uint8_t>('<') ||
        d[q + 1] != static_cast<std::uint8_t>('<')) {
        local.status = CMapStatus::BadObject;
        local.message = "stream dictionary missing";
        if (report != nullptr) {
            *report = local;
        }
        return nullptr;
    }
    const std::size_t close = parser::scan::matching_dict_close(d, q, size);
    if (close == parser::scan::kNpos) {
        local.status = CMapStatus::BadObject;
        local.message = "stream dictionary unterminated";
        if (report != nullptr) {
            *report = local;
        }
        return nullptr;
    }

    std::uint64_t length = 0;
    bool has_length = false;
    parser::stream::FilterClass filter;
    bool predictor = false;
    parser::scan::for_each_dict_entry(
        d, q + 2, close,
        [&](std::string_view key, std::size_t vp, std::size_t ve) {
            if (key == "Length" && !has_length) {
                // Direct integer only — an indirect /Length cannot be
                // resolved before an object table exists (P-021).
                if (parser::stream::parse_direct_integer(d, vp, ve,
                                                          length)) {
                    has_length = true;
                }
            } else if (key == "Filter") {
                filter = parser::stream::classify_filter(d, vp, ve);
            } else if (key == "DecodeParms") {
                predictor =
                    parser::stream::parms_demand_predictor(d, vp, ve);
            }
        });

    const std::size_t data_start =
        parser::stream::locate_stream_data(d, size, close + 2);
    if (data_start == parser::scan::kNpos || data_start >= size) {
        local.status = CMapStatus::BadObject;
        local.message = "stream payload missing";
        if (report != nullptr) {
            *report = local;
        }
        return nullptr;
    }

    // Payload extent. Flate: a direct /Length bounds the compressed
    // bytes (searching inside compressed data is not sound). Raw: the
    // first `endstream` marker (the parser stops at endcmap anyway);
    // /Length is advisory for text and the marker is the more reliable
    // boundary in practice — deterministic either way, which is all
    // the dedup key requires.
    if (filter.has_filter && !(filter.flate_only && !predictor)) {
        local.status = CMapStatus::UnsupportedFilter;
        local.message =
            "filter pipeline not decoded (non-Flate, multi-filter or "
            "predictor)";
        if (report != nullptr) {
            *report = local;
        }
        return nullptr;
    }
    std::size_t extent = 0;
    if (filter.flate_only) {
        if (!has_length) {
            local.status = CMapStatus::IndirectLength;
            local.message = "Flate payload needs a direct /Length";
            if (report != nullptr) {
                *report = local;
            }
            return nullptr;
        }
        const std::uint64_t avail =
            static_cast<std::uint64_t>(size - data_start);
        extent = length < avail ? static_cast<std::size_t>(length)
                                : static_cast<std::size_t>(avail);
    } else {
        extent = find_endstream(d, data_start, size) - data_start;
    }
    if (extent == 0) {
        local.status = CMapStatus::BadObject;
        local.message = "empty stream payload";
        if (report != nullptr) {
            *report = local;
        }
        return nullptr;
    }
    const std::span<const std::uint8_t> payload(d + data_start, extent);

    // Dedup: xxHash64 over the payload, seeded per class. A hit
    // requires byte equality — collisions can never merge different
    // CMaps, and differing payloads with equal hashes coexist.
    const std::uint64_t key = hash::xxhash64(
        payload,
        filter.flate_only ? kSeedFlatePayload : kSeedRawPayload);
    for (const Entry& e : entries_) {
        if (e.hash == key && e.payload.size() == payload.size() &&
            std::equal(e.payload.begin(), e.payload.end(),
                       payload.begin())) {
            ++intern_hits_;
            local.cache_hit = true;
            local.message = "reused interned CMap";
            if (report != nullptr) {
                *report = local;
            }
            return e.table;
        }
    }

    // Miss: obtain the CMap text.
    std::span<const std::uint8_t> text = payload;
    if (filter.flate_only) {
        if (!codec::FlateDecompressor::supported()) {
            local.status = CMapStatus::FlateUnavailable;
            local.message = "built without the Flate codec (offline)";
            if (report != nullptr) {
                *report = local;
            }
            return nullptr;
        }
        if (!scratch_.has_value()) {
            scratch_.emplace(kFlateScratchBytes);
        }
        if (!scratch_->valid()) {
            local.status = CMapStatus::FlateFailed;
            local.message = "scratch arena allocation failed";
            if (report != nullptr) {
                *report = local;
            }
            return nullptr;
        }
        const codec::FlateResult out =
            decompressor_.decompress(payload, *scratch_,
                                     codec::FlateLimits{});
        if (out.status != codec::FlateStatus::Ok) {
            local.status = CMapStatus::FlateFailed;
            local.message = "Flate payload refused (corrupt or bomb)";
            if (report != nullptr) {
                *report = local;
            }
            return nullptr;
        }
        text = out.output;
    }

    const std::shared_ptr<const CMapTable> table =
        CMapTable::parse(text, &local.parse_stats);
    if (filter.flate_only && scratch_.has_value()) {
        // The table copied what it keeps; staging is dead.
        scratch_->rewind_to(0);
    }

    entries_.push_back(Entry{
        key, std::vector<std::uint8_t>(payload.begin(), payload.end()),
        table});
    local.message = "parsed and interned";
    if (report != nullptr) {
        *report = local;
    }
    return table;
}

}  // namespace pdftoolkit::font
