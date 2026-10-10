#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace pdftoolkit::layout {

/// A 6-element PDF affine matrix `[a b c d e f]` in PDF's row-vector
/// convention (audit task 3.2: "affine matrices in CPU registers using
/// std::array<float, 6>"):
///
///   [x' y' 1] = [x y 1] . M  =>  x' = a*x + c*y + e
///                                   y' = b*x + d*y + f
///
/// Products compose RIGHT-to-left in application order: mat_mul(A, B)
/// is "apply A first, then B" — the convention every PDF operator
/// description uses (e.g. `cm` pre-multiplies: CTM' = M_cm x CTM).
using Mat6 = std::array<float, 6>;

inline constexpr Mat6 kMat6Identity{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F};

/// Matrix product `first`-then-`then` (see Mat6 for the convention).
/// Pure function; the evaluator's hot path uses a double-precision
/// internal variant (ADR-0009) but composes the same algebra.
[[nodiscard]] constexpr Mat6 mat_mul(const Mat6& first,
                                     const Mat6& then) noexcept {
    return Mat6{
        first[0] * then[0] + first[1] * then[2],
        first[0] * then[1] + first[1] * then[3],
        first[2] * then[0] + first[3] * then[2],
        first[2] * then[1] + first[3] * then[3],
        first[4] * then[0] + first[5] * then[2] + then[4],
        first[4] * then[1] + first[5] * then[3] + then[5],
    };
}

/// PDF text state (ISO 32000-1:2008 Table 51/104 — the half that q/Q
/// saves and restores). Every field is stored exactly as the operand
/// wrote it (float32, the token's parsed value); unit conversion
/// happens at point of use (e.g. Tz is a percentage, used as /100).
struct TextState {
    float char_spacing = 0.0F;   // Tc — points, every code
    float word_spacing = 0.0F;   // Tw — points, single-byte code 32 only
    float h_scale_pct = 100.0F;  // Tz — PERCENT as written (100 = none)
    float leading = 0.0F;        // TL — points (TD sets -ty)
    float font_size = 0.0F;      // Tfs — the Tf size operand
    float rise = 0.0F;           // Ts — points
    int render_mode = 0;         // Tr — tracked, geometry-neutral
    /// Active font resource name (Tf's name operand, the raw token text
    /// after the '/', `#`-escapes intact — same contract as every Name
    /// token since task 2.3). Empty = no font selected yet. Zero-copy:
    /// points into the evaluated content buffer and is only valid while
    /// that buffer lives.
    std::string_view font;
};

/// One glyph event, emitted through GlyphSink per shown character code
/// (the seam task 3.3 consumes: it resolves `code` through CMapTable
/// and transforms coordinates with `trm`).
struct GlyphPlacement {
    /// Packed character code (1 or 2 bytes per the active font's code
    /// width — the FontMetricsResolver's answer).
    std::uint32_t code = 0;
    /// Active font resource name (zero-copy into the content buffer).
    std::string_view font;
    /// Text rendering matrix (ISO 32000-1:2008 9.4.4):
    /// Trm = [Tfs*Th 0 0 Tfs 0 Ts] x Tm x CTM — device space.
    Mat6 trm{};
    /// This glyph's horizontal displacement in TEXT space (points):
    /// tx = (w0/1000)*Tfs*Th + Tc + Tw(applied) — the exact quantity
    /// the position advanced by after painting this glyph.
    float advance_tx = 0.0F;
};

/// Consumer of per-glyph events (task 3.3's input). Implementations
/// must not throw and must not retain `g` beyond the call (its `font`
/// view points into the content buffer).
class GlyphSink {
public:
    virtual void on_glyph(const GlyphPlacement& g) noexcept = 0;

protected:
    ~GlyphSink() = default;
};

/// Font metrics the evaluator needs but cannot resolve itself: real
/// widths live in font dictionaries behind indirect references
/// (/Widths, Type0 /W arrays) and the object-graph resolver that would
/// plumb them is not landed (P-023). This seam is its docking point.
///
/// Callback contract (both must be noexcept and total):
///   * `code_bytes` — width of one character code in bytes for the
///     font resource `font` (1 = simple, 2 = CID/Type0 typical).
///   * `advance`    — w0, the glyph's horizontal displacement in
///     thousandths of a unit of text space.
///
/// Default-constructed (null functions): 1-byte codes and w0 = 1000 —
/// the ISO 32000-1 /DW default for CIDFonts, applied to every font as
/// the documented interim until P-023 lands (problem registry P-025).
struct FontMetricsResolver {
    unsigned (*code_bytes)(void* ctx, std::string_view font) noexcept =
        nullptr;
    std::uint32_t (*advance)(void* ctx, std::string_view font,
                             std::uint32_t code) noexcept = nullptr;
    void* ctx = nullptr;
};

/// Tolerant-evaluation accounting (evidence + test surface). Every
/// counter is monotonic across evaluate() calls.
struct EvaluatorStats {
    /// Keyword tokens dispatched in normal mode (known + unknown).
    std::size_t ops_dispatched = 0;
    /// Dispatched operators this evaluator does not know (operands
    /// dropped — the tolerant contract for graphics ops like `d`/`m`).
    std::size_t ops_unknown = 0;
    /// Positioning/showing operators ignored because no BT is active
    /// (ISO 32000-1 leaves their out-of-text behaviour undefined; the
    /// conservative choice is to ignore — P-025).
    std::size_t ops_outside_text = 0;
    std::size_t strings_shown = 0;
    std::size_t glyphs_emitted = 0;
    /// TJ arrays replayed (array followed by the TJ operator).
    std::size_t tj_arrays_replayed = 0;
    /// Number elements consumed inside replayed TJ arrays.
    std::size_t tj_number_adjustments = 0;
    /// `]` with no array open / `>>` with no dict open (tolerated).
    std::size_t stray_array_ends = 0;
    std::size_t stray_dict_ends = 0;
    /// Operands dropped because the fixed accumulator was full
    /// (> 8 numbers, or a 2nd name/string).
    std::size_t operand_overflows = 0;
    /// `q` beyond the fixed 64-frame gstate stack: state not saved.
    std::size_t gstate_overflow = 0;
    /// `Q` on an empty gstate stack: ignored.
    std::size_t gstate_underflow = 0;
    /// Trailing odd byte(s) of a string under a 2-byte code width.
    std::size_t unterminated_code_tail = 0;
    /// Stray stream/endstream tokens inside the content stream.
    std::size_t stray_stream_tokens = 0;
};

/// Content stream operator state machine (audit task 3.2): a
/// push-down evaluator for text-rendering operators that tracks affine
/// transformations, font state and coordinate vectors with ZERO heap
/// allocation — all state is fixed-size members, every token is
/// borrowed, and evaluate() is noexcept.
///
/// Operator scope (everything else is tolerated and skipped):
///   * text state (q/Q-saveable): Tc Tw Tz TL Tf Tr Ts
///   * text positioning: BT ET Td TD Tm T*
///   * text showing: Tj ' " TJ (kerning via TJ numbers + Tc/Tw)
///   * graphics state: q Q cm (CTM is otherwise unreachable)
///
/// Precision model (ADR-0009): the exposed matrices are the audit's
/// float32 Mat6, but composition carries double-precision state — a
/// float32 Tm/line-matrix accumulator drifts past the task's +/-0.001
/// pt gate on long kerned lines (measured ~1.2e-3 pt over 400 glyphs),
/// while the double carry stays ~1e-12. Glyph events round to float at
/// the API boundary; the advance math is double end-to-end.
///
/// Semantics notes (ISO 32000-1:2008 9.3-9.4, deviations in P-025):
///   * glyph advance updates Tm but NEVER the line matrix Tlm — Td
///     composes with Tlm, which is why Td(0,0) returns to the line
///     start after a Tj;
///   * TJ arrays are recorded as byte ranges and replayed only when
///     the TJ operator follows — `[3 2] 0 d` must not move the pen;
///   * keywords inside dictionaries (R, true, false, null) and arrays
///     are members, never operators;
///   * text-positioning/showing operators between ET and the next BT
///     are ignored and counted;
///   * Tw applies only to the single-byte code 32 (never to a byte of
///     a 2-byte code), Tc to every code;
///   * a positive TJ number moves the following text LEFT: its
///     displacement is -(Tj/1000)*Tfs*Th (ISO 9.4.3's w0-Tj form).
///
/// Lifecycle: one evaluator per logical stream sequence. evaluate()
/// may be called repeatedly to CONTINUE (PDF content arrays are
/// concatenated then interpreted — state, including the gstate stack
/// and any pending TJ array, persists across calls). reset() restarts
/// from constructor state (cheap: only the scalars, never the
/// fixed-size gstate storage, which is written before it is read).
class OperatorEvaluator {
public:
    /// Fixed graphics-state stack depth (q/Q frames). Unbalanced `q`
    /// beyond this depth is ignored and counted (P-025 scope line).
    static constexpr unsigned kMaxGStateDepth = 64;

    explicit OperatorEvaluator(
        GlyphSink* sink = nullptr,
        const FontMetricsResolver& fonts = FontMetricsResolver{}) noexcept;

    /// Evaluates one content-stream segment (already decompressed —
    /// Flate is the caller's concern, tasks 2.4/3.1). Never allocates,
    /// never throws, always terminates (the lexer terminates and every
    /// loop is bounded); hostile input degrades tolerantly per the
    /// stats counters. The buffer must outlive glyph-event use.
    void evaluate(std::span<const std::uint8_t> content) noexcept;

    /// Returns to constructor state (see the lifecycle note above).
    void reset() noexcept;

    // --- introspection (tests, task 3.3 wiring, the precision gate) ---

    /// Current transformation matrix (float view of the double carry).
    [[nodiscard]] Mat6 ctm() const noexcept;
    /// Text line matrix Tlm (positioning operators compose here).
    [[nodiscard]] Mat6 text_line_matrix() const noexcept;
    /// Text matrix Tm — Tlm advanced by the current glyph-run
    /// displacement (materialised on demand).
    [[nodiscard]] Mat6 text_matrix() const noexcept;
    [[nodiscard]] const TextState& text_state() const noexcept {
        return text_;
    }
    [[nodiscard]] bool in_text_object() const noexcept {
        return in_text_;
    }
    [[nodiscard]] const EvaluatorStats& stats() const noexcept {
        return stats_;
    }

    OperatorEvaluator(const OperatorEvaluator&) = delete;
    OperatorEvaluator& operator=(const OperatorEvaluator&) = delete;
    OperatorEvaluator(OperatorEvaluator&&) = delete;
    OperatorEvaluator& operator=(OperatorEvaluator&&) = delete;

private:
    struct GraphicsState {
        std::array<double, 6> ctm;  // double carry (ADR-0009)
        TextState text;
    };

    // --- token plumbing (evaluator.cpp) ---
    void on_keyword(std::string_view kw) noexcept;
    void show_string(std::string_view raw, bool is_hex) noexcept;
    void replay_tj() noexcept;
    void apply_td(double tx, double ty) noexcept;
    void set_text_matrix(const std::array<double, 6>& m) noexcept;
    void refresh_positional_carry() noexcept;
    unsigned font_code_bytes() const noexcept;
    std::uint32_t font_advance(std::string_view font,
                               std::uint32_t code) const noexcept;
    void clear_pending_operands() noexcept;

    GlyphSink* sink_;
    FontMetricsResolver fonts_;

    // Positional state. Invariant inside BT..ET: Tm == T_tx(D) x Tlm,
    // where D is the accumulated glyph displacement since the last
    // positioning operator (the double carry, ADR-0009). P is
    // Tlm x CTM in doubles, refreshed at the top of every showing
    // operator (single choke point — no stale-P bug class).
    std::array<double, 6> ctm_d_;
    std::array<double, 6> tlm_d_;
    std::array<double, 6> p_;
    double displacement_;
    bool in_text_;
    TextState text_;

    // q/Q stack. gstack_ frames are written before they are read and
    // are never zero-initialised by design (reset() stays cheap).
    GraphicsState gstack_[kMaxGStateDepth];
    unsigned gsp_;

    // Operand accumulator (fixed, drop-on-overflow — see stats).
    static constexpr unsigned kMaxOperands = 8;
    double operands_[kMaxOperands];
    unsigned n_operands_;
    std::string_view name_operand_;
    std::string_view string_operand_;
    bool string_is_hex_;
    bool has_name_;
    bool has_string_;

    // Array region recorder: a completed array's element byte range
    // awaits its operator; TJ replays it, anything else drops it.
    const std::uint8_t* array_base_;  // buffer the range belongs to
    std::size_t array_begin_;         // just past '['
    std::size_t array_end_;           // at ']'
    bool array_pending_;

    // Skip-mode nesting (tokens inside are members, never operands).
    unsigned array_depth_;  // > 0: inside an array being skipped
    unsigned dict_depth_;   // > 0: inside a dict being skipped

    const std::uint8_t* base_;  // current evaluate() buffer (for spans)
    std::size_t base_size_;

    EvaluatorStats stats_;
};

}  // namespace pdftoolkit::layout
