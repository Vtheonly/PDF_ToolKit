// Content stream operator state machine (audit issue-1/task-3.2).
//
// A push-down evaluator over the task-2.3 ZeroCopyLexer that tracks
// the text-rendering state a content stream defines (ISO 32000-1:2008
// 9.3-9.4) and emits one GlyphPlacement per shown character code —
// the exact event stream task 3.3's slab population consumes.
//
// Semantics implemented (spec-first; the golden-reference generator
// scripts/gen_evaluator_golden.py models the same rules independently
// in Python and its vectors gate this file to +/-0.001 pt):
//
//   * Tm/Tlm: BT resets both to identity; Td pre-multiplies a
//     translation into Tlm (glyph advances NEVER touch Tlm — that is
//     why Td(0,0) rewinds to the line start after a Tj); Tm replaces
//     both; T* is Td(0, -TL); TD sets TL = -ty first.
//   * Glyph advance (horizontal writing): tx = (w0/1000)*Tfs*Th +
//     Tc + Tw(single-byte code 32 only); a TJ number's displacement is
//     -(Tj/1000)*Tfs*Th. The displacement accumulates in a DOUBLE
//     carry D with Tm == T_tx(D) x Tlm (ADR-0009 — a float32 carry
//     drifts past the gate on long kerned lines).
//   * Trm per glyph = [Tfs*Th 0 0 Tfs 0 Ts] x T_tx(D) x Tlm x CTM,
//     evaluated as the 14-flop closed form against P = Tlm x CTM
//     (refreshed once per showing operator — the single choke point).
//   * q/Q save CTM + text state (not Tm/Tlm — text-object-scoped);
//     cm pre-multiplies CTM.
//   * Tolerant skipping: unknown operators drop their operands;
//     keywords inside dictionaries (R/true/false/null) and arrays are
//     members, never operators; an array's byte range is recorded and
//     replayed ONLY for TJ (a `[3 2] 0 d` dash pattern must never move
//     the pen); stray brackets and unterminated constructs are
//     counted, never fatal (the 2.1-3.1 house contract).

#include "pdftoolkit/layout/evaluator.hpp"

#include "pdftoolkit/parser/lexer.hpp"

namespace pdftoolkit::layout {
namespace {

using parser::PdfToken;
using parser::ZeroCopyLexer;

using Mat6d = std::array<double, 6>;

constexpr Mat6d kIdentity6d{1.0, 0.0, 0.0, 1.0, 0.0, 0.0};

// Double-precision product (the public mat_mul is the float32 form).
constexpr Mat6d mat_mul_d(const Mat6d& first, const Mat6d& then) noexcept {
    return Mat6d{
        first[0] * then[0] + first[1] * then[2],
        first[0] * then[1] + first[1] * then[3],
        first[2] * then[0] + first[3] * then[2],
        first[2] * then[1] + first[3] * then[3],
        first[4] * then[0] + first[5] * then[2] + then[4],
        first[4] * then[1] + first[5] * then[3] + then[5],
    };
}

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

// Token numeric value as double. Integer tokens keep int64 precision
// (operands beyond 2^24 are not f32-representable; the golden
// generator bounds its literals so both sides agree exactly).
double token_number(const PdfToken& t) noexcept {
    if (t.type == PdfToken::Type::Integer) {
        return static_cast<double>(t.int_val);
    }
    return static_cast<double>(t.float_val);
}

Mat6 round_to_mat6(const Mat6d& m) noexcept {
    return Mat6{static_cast<float>(m[0]), static_cast<float>(m[1]),
                static_cast<float>(m[2]), static_cast<float>(m[3]),
                static_cast<float>(m[4]), static_cast<float>(m[5])};
}

}  // namespace

OperatorEvaluator::OperatorEvaluator(GlyphSink* sink,
                                     const FontMetricsResolver& fonts) noexcept
    : sink_(sink), fonts_(fonts) {
    reset();
}

void OperatorEvaluator::reset() noexcept {
    // Scalars only: gstack_ frames are written before they are read
    // (push writes the frame, pop reads what a push wrote) — zero-
    // initialising 64 frames per stream would charge every evaluation
    // a cost the semantics never need.
    ctm_d_ = kIdentity6d;
    tlm_d_ = kIdentity6d;
    p_ = kIdentity6d;
    displacement_ = 0.0;
    in_text_ = false;
    text_ = TextState{};
    gsp_ = 0;
    n_operands_ = 0;
    name_operand_ = {};
    string_operand_ = {};
    string_is_hex_ = false;
    has_name_ = false;
    has_string_ = false;
    array_begin_ = 0;
    array_end_ = 0;
    array_base_ = nullptr;
    array_pending_ = false;
    array_depth_ = 0;
    dict_depth_ = 0;
    base_ = nullptr;
    base_size_ = 0;
    stats_ = EvaluatorStats{};
}

// --- introspection ----------------------------------------------------------

Mat6 OperatorEvaluator::ctm() const noexcept { return round_to_mat6(ctm_d_); }

Mat6 OperatorEvaluator::text_line_matrix() const noexcept {
    return round_to_mat6(tlm_d_);
}

Mat6 OperatorEvaluator::text_matrix() const noexcept {
    // Tm == T_tx(D) x Tlm, closed form (the class invariant).
    return Mat6{static_cast<float>(tlm_d_[0]), static_cast<float>(tlm_d_[1]),
                static_cast<float>(tlm_d_[2]), static_cast<float>(tlm_d_[3]),
                static_cast<float>(displacement_ * tlm_d_[0] + tlm_d_[4]),
                static_cast<float>(displacement_ * tlm_d_[1] + tlm_d_[5])};
}

// --- font seam (P-023 docking point) ----------------------------------------

unsigned OperatorEvaluator::font_code_bytes() const noexcept {
    if (fonts_.code_bytes == nullptr || text_.font.empty()) {
        return 1;
    }
    const unsigned cb = fonts_.code_bytes(fonts_.ctx, text_.font);
    return cb == 2 ? 2 : 1;  // only 1 and 2 are in scope (audit 3.1/3.2)
}

std::uint32_t OperatorEvaluator::font_advance(
    std::string_view font, std::uint32_t code) const noexcept {
    if (fonts_.advance == nullptr || font.empty()) {
        return 1000;  // ISO 32000-1 /DW default (P-025)
    }
    return fonts_.advance(fonts_.ctx, font, code);
}

// --- positioning ------------------------------------------------------------

void OperatorEvaluator::refresh_positional_carry() noexcept {
    p_ = mat_mul_d(tlm_d_, ctm_d_);
}

void OperatorEvaluator::apply_td(double tx, double ty) noexcept {
    const Mat6d translate{1.0, 0.0, 0.0, 1.0, tx, ty};
    tlm_d_ = mat_mul_d(translate, tlm_d_);
    displacement_ = 0.0;  // Tm = Tlm
}

void OperatorEvaluator::set_text_matrix(const Mat6d& m) noexcept {
    tlm_d_ = m;
    displacement_ = 0.0;  // Tm = Tlm
}

// --- text showing -----------------------------------------------------------

void OperatorEvaluator::show_string(std::string_view raw,
                                    bool is_hex) noexcept {
    if (!in_text_) {
        ++stats_.ops_outside_text;  // backstop; dispatch pre-checks
        return;
    }
    ++stats_.strings_shown;

    // Single choke point: CTM/Tlm may have changed since the last
    // showing operator. Text-space scalars for this string: state
    // cannot change inside a string (no operators can occur there).
    refresh_positional_carry();
    const double fs = static_cast<double>(text_.font_size);
    const double hz = static_cast<double>(text_.h_scale_pct) / 100.0;
    const double tc = static_cast<double>(text_.char_spacing);
    const double tw = static_cast<double>(text_.word_spacing);
    const double rise = static_cast<double>(text_.rise);
    const double sx = fs * hz;  // Trm a/b scale
    const double sy = fs;       // Trm c/d scale
    const unsigned code_bytes = font_code_bytes();

    // Code assembly shift register + per-glyph emission. Decode and
    // assembly are fused: one pass over the raw bytes, no
    // materialisation, no recursion.
    std::uint32_t code_acc = 0;
    unsigned code_fill = 0;

    const auto emit_code = [&](std::uint32_t code) noexcept {
        const std::uint32_t w0 = font_advance(text_.font, code);
        const bool tw_applies = code_bytes == 1 && code == 32;
        const double tx = static_cast<double>(w0) / 1000.0 * fs * hz + tc +
                          (tw_applies ? tw : 0.0);

        GlyphPlacement g;
        g.code = code;
        g.font = text_.font;
        g.trm = Mat6{
            static_cast<float>(sx * p_[0]),
            static_cast<float>(sx * p_[1]),
            static_cast<float>(sy * p_[2]),
            static_cast<float>(sy * p_[3]),
            static_cast<float>(displacement_ * p_[0] + rise * p_[2] + p_[4]),
            static_cast<float>(displacement_ * p_[1] + rise * p_[3] + p_[5])};
        g.advance_tx = static_cast<float>(tx);
        if (sink_ != nullptr) {
            sink_->on_glyph(g);
        }
        ++stats_.glyphs_emitted;
        displacement_ += tx;  // AFTER the glyph is placed
    };

    const auto feed_byte = [&](std::uint8_t b) noexcept {
        if (code_bytes == 1) {
            emit_code(b);
            return;
        }
        code_acc = (code_acc << 8) | b;
        ++code_fill;
        if (code_fill == 2) {
            emit_code(code_acc);
            code_acc = 0;
            code_fill = 0;
        }
    };

    if (is_hex) {
        // Body: between the outer '<' and '>'; whitespace ignored;
        // odd trailing digit padded with 0; non-hex bytes skipped.
        std::string_view body = raw;
        if (!body.empty() && body.front() == '<') {
            body.remove_prefix(1);
        }
        if (!body.empty() && body.back() == '>') {
            body.remove_suffix(1);
        }
        int hi = -1;
        for (const char c : body) {
            const int nib = hex_nibble(c);
            if (nib < 0) {
                continue;  // whitespace or junk — tolerated
            }
            if (hi < 0) {
                hi = nib;
            } else {
                feed_byte(static_cast<std::uint8_t>((hi << 4) | nib));
                hi = -1;
            }
        }
        if (hi >= 0) {
            feed_byte(static_cast<std::uint8_t>(hi << 4));  // pad low 0
        }
    } else {
        // Body: between the outer '(' and ')' — escapes decoded on the
        // fly per ISO 32000-1 7.3.4.2 (\n \r \t \b \f \( \ ) \\ \ddd,
        // line continuations produce nothing, unknown escapes drop
        // the backslash; \ddd values beyond 255 wrap mod 256).
        std::string_view body = raw;
        if (!body.empty() && body.front() == '(') {
            body.remove_prefix(1);
        }
        if (!body.empty() && body.back() == ')') {
            body.remove_suffix(1);
        }
        const std::size_t n = body.size();
        std::size_t i = 0;
        while (i < n) {
            const std::uint8_t c = static_cast<std::uint8_t>(body[i++]);
            if (c != static_cast<std::uint8_t>('\\')) {
                feed_byte(c);
                continue;
            }
            if (i >= n) {
                break;  // trailing backslash: no output byte
            }
            const std::uint8_t e = static_cast<std::uint8_t>(body[i++]);
            switch (e) {
                case '\n':
                    break;  // line continuation
                case '\r':
                    if (i < n && body[i] == '\n') {
                        ++i;  // CRLF is ONE continuation
                    }
                    break;
                case 'n':
                    feed_byte(static_cast<std::uint8_t>('\n'));
                    break;
                case 'r':
                    feed_byte(static_cast<std::uint8_t>('\r'));
                    break;
                case 't':
                    feed_byte(static_cast<std::uint8_t>('\t'));
                    break;
                case 'b':
                    feed_byte(0x08);
                    break;
                case 'f':
                    feed_byte(0x0C);
                    break;
                case '(':
                    feed_byte(static_cast<std::uint8_t>('('));
                    break;
                case ')':
                    feed_byte(static_cast<std::uint8_t>(')'));
                    break;
                case '\\':
                    feed_byte(static_cast<std::uint8_t>('\\'));
                    break;
                default: {
                    if (e >= '0' && e <= '7') {
                        std::uint32_t val =
                            static_cast<std::uint32_t>(e - '0');
                        unsigned digits = 1;
                        while (digits < 3 && i < n) {
                            const char d = body[i];
                            if (d < '0' || d > '7') {
                                break;
                            }
                            val = val * 8U +
                                  static_cast<std::uint32_t>(d - '0');
                            ++i;
                            ++digits;
                        }
                        feed_byte(static_cast<std::uint8_t>(val & 0xFFU));
                    } else {
                        // Unknown escape: the backslash is ignored.
                        feed_byte(e);
                    }
                    break;
                }
            }
        }
    }

    if (code_fill != 0) {
        // Odd trailing byte(s) under a 2-byte code width: dropped.
        stats_.unterminated_code_tail += code_fill;
    }
}

void OperatorEvaluator::replay_tj() noexcept {
    // Re-lex the recorded array element range (strictly between the
    // brackets). Strings show glyphs; numbers adjust the displacement;
    // nested arrays are skipped with a depth counter; anything else is
    // a tolerated member. Deterministic: same bytes, same lexer.
    if (array_base_ == nullptr || array_end_ <= array_begin_ ||
        array_end_ > base_size_) {
        return;
    }
    ++stats_.tj_arrays_replayed;

    refresh_positional_carry();
    const double fs = static_cast<double>(text_.font_size);
    const double hz = static_cast<double>(text_.h_scale_pct) / 100.0;

    ZeroCopyLexer lexer(std::span<const std::uint8_t>(
        array_base_ + array_begin_, array_end_ - array_begin_));
    unsigned depth = 0;
    for (;;) {
        const PdfToken t = lexer.next();
        switch (t.type) {
            case PdfToken::Type::EndOfFile:
                return;
            case PdfToken::Type::ArrayStart:
                ++depth;  // nested: its members are skipped
                break;
            case PdfToken::Type::ArrayEnd:
                if (depth > 0) {
                    --depth;
                }
                break;
            case PdfToken::Type::Integer:
            case PdfToken::Type::Real:
                if (depth == 0) {
                    // ISO 9.4.3: a TJ number's displacement is
                    // -(Tj/1000)*Tfs*Th (positive moves LEFT).
                    displacement_ -= token_number(t) / 1000.0 * fs * hz;
                    ++stats_.tj_number_adjustments;
                }
                break;
            case PdfToken::Type::StringLit:
                if (depth == 0) {
                    show_string(t.value, false);
                }
                break;
            case PdfToken::Type::HexStr:
                if (depth == 0) {
                    show_string(t.value, true);
                }
                break;
            default:
                break;  // names/keywords/braces/dict tokens: tolerated
        }
    }
}

// --- operand accumulator ----------------------------------------------------

void OperatorEvaluator::clear_pending_operands() noexcept {
    n_operands_ = 0;
    name_operand_ = {};
    string_operand_ = {};
    string_is_hex_ = false;
    has_name_ = false;
    has_string_ = false;
    array_pending_ = false;
}

// --- operator dispatch ------------------------------------------------------

void OperatorEvaluator::on_keyword(std::string_view kw) noexcept {
    ++stats_.ops_dispatched;

    // Ordered roughly by content-stream frequency.
    if (kw == "Tj") {
        if (!in_text_) {
            ++stats_.ops_outside_text;
        } else if (has_string_) {
            show_string(string_operand_, string_is_hex_);
        }
    } else if (kw == "TJ") {
        if (!in_text_) {
            ++stats_.ops_outside_text;
        } else if (array_pending_) {
            replay_tj();
        }
    } else if (kw == "Td") {
        if (!in_text_) {
            ++stats_.ops_outside_text;
        } else if (n_operands_ >= 2) {
            apply_td(operands_[0], operands_[1]);
        }
    } else if (kw == "TD") {
        if (!in_text_) {
            ++stats_.ops_outside_text;
        } else if (n_operands_ >= 2) {
            text_.leading = static_cast<float>(-operands_[1]);
            apply_td(operands_[0], operands_[1]);
        }
    } else if (kw == "Tm") {
        if (!in_text_) {
            ++stats_.ops_outside_text;
        } else if (n_operands_ >= 6) {
            set_text_matrix(Mat6d{operands_[0], operands_[1], operands_[2],
                                  operands_[3], operands_[4], operands_[5]});
        }
    } else if (kw == "Tf") {
        if (has_name_ && n_operands_ >= 1) {
            text_.font = name_operand_;
            text_.font_size = static_cast<float>(operands_[0]);
        }
    } else if (kw == "Tc") {
        if (n_operands_ >= 1) {
            text_.char_spacing = static_cast<float>(operands_[0]);
        }
    } else if (kw == "Tw") {
        if (n_operands_ >= 1) {
            text_.word_spacing = static_cast<float>(operands_[0]);
        }
    } else if (kw == "Tz") {
        if (n_operands_ >= 1) {
            text_.h_scale_pct = static_cast<float>(operands_[0]);
        }
    } else if (kw == "TL") {
        if (n_operands_ >= 1) {
            text_.leading = static_cast<float>(operands_[0]);
        }
    } else if (kw == "Tr") {
        if (n_operands_ >= 1) {
            text_.render_mode = static_cast<int>(operands_[0]);
        }
    } else if (kw == "Ts") {
        if (n_operands_ >= 1) {
            text_.rise = static_cast<float>(operands_[0]);
        }
    } else if (kw == "T*") {
        if (!in_text_) {
            ++stats_.ops_outside_text;
        } else {
            apply_td(0.0, -static_cast<double>(text_.leading));
        }
    } else if (kw == "'") {
        if (!in_text_) {
            ++stats_.ops_outside_text;
        } else if (has_string_) {
            apply_td(0.0, -static_cast<double>(text_.leading));
            show_string(string_operand_, string_is_hex_);
        }
    } else if (kw == "\"") {
        if (!in_text_) {
            ++stats_.ops_outside_text;
        } else if (has_string_ && n_operands_ >= 2) {
            text_.word_spacing = static_cast<float>(operands_[0]);
            text_.char_spacing = static_cast<float>(operands_[1]);
            apply_td(0.0, -static_cast<double>(text_.leading));
            show_string(string_operand_, string_is_hex_);
        }
    } else if (kw == "BT") {
        in_text_ = true;
        set_text_matrix(kIdentity6d);
    } else if (kw == "ET") {
        in_text_ = false;  // Tm/Tlm persist until the next BT (9.4.1)
    } else if (kw == "q") {
        if (gsp_ < kMaxGStateDepth) {
            GraphicsState& frame = gstack_[gsp_++];
            frame.ctm = ctm_d_;
            frame.text = text_;
        } else {
            ++stats_.gstate_overflow;
        }
    } else if (kw == "Q") {
        if (gsp_ > 0) {
            const GraphicsState& frame = gstack_[--gsp_];
            ctm_d_ = frame.ctm;
            text_ = frame.text;
        } else {
            ++stats_.gstate_underflow;
        }
    } else if (kw == "cm") {
        if (n_operands_ >= 6) {
            const Mat6d m{operands_[0], operands_[1], operands_[2],
                          operands_[3], operands_[4], operands_[5]};
            ctm_d_ = mat_mul_d(m, ctm_d_);  // pre-multiplied (8.4.2)
        }
    } else {
        ++stats_.ops_unknown;
    }

    clear_pending_operands();
}

// --- main loop --------------------------------------------------------------

void OperatorEvaluator::evaluate(
    std::span<const std::uint8_t> content) noexcept {
    base_ = content.data();
    base_size_ = content.size();

    // Continuation semantics: STATE persists across calls (matrices,
    // text state, gstate stack, in-text flag, displacement); LEXICAL
    // position does not — each call starts at top level, and operands
    // must live in the same segment as their operator. A completed
    // array awaiting TJ survives only when it was recorded in this
    // very buffer (otherwise its byte range is meaningless).
    if (array_pending_ && array_base_ != base_) {
        array_pending_ = false;
    }
    array_depth_ = 0;
    dict_depth_ = 0;
    n_operands_ = 0;
    has_name_ = false;
    has_string_ = false;
    name_operand_ = {};
    string_operand_ = {};

    ZeroCopyLexer lexer(content);
    for (;;) {
        const PdfToken t = lexer.next();
        switch (t.type) {
            case PdfToken::Type::EndOfFile:
                return;

            case PdfToken::Type::Keyword:
                if (array_depth_ > 0 || dict_depth_ > 0) {
                    break;  // member, never an operator
                }
                on_keyword(t.value);
                break;

            case PdfToken::Type::Integer:
            case PdfToken::Type::Real:
                if (array_depth_ > 0 || dict_depth_ > 0) {
                    break;
                }
                if (n_operands_ < kMaxOperands) {
                    operands_[n_operands_++] = token_number(t);
                } else {
                    ++stats_.operand_overflows;
                }
                break;

            case PdfToken::Type::Name:
                if (array_depth_ > 0 || dict_depth_ > 0) {
                    break;
                }
                if (!has_name_) {
                    name_operand_ = t.value.substr(1);  // drop the '/'
                    has_name_ = true;
                } else {
                    ++stats_.operand_overflows;
                }
                break;

            case PdfToken::Type::StringLit:
            case PdfToken::Type::HexStr:
                if (array_depth_ > 0 || dict_depth_ > 0) {
                    break;
                }
                if (!has_string_) {
                    string_operand_ = t.value;
                    string_is_hex_ = t.type == PdfToken::Type::HexStr;
                    has_string_ = true;
                } else {
                    ++stats_.operand_overflows;
                }
                break;

            case PdfToken::Type::ArrayStart:
                if (dict_depth_ > 0) {
                    break;  // dict member
                }
                if (array_depth_ == 0) {
                    // A completed-but-unclaimed array is superseded.
                    array_pending_ = false;
                    array_begin_ = lexer.offset();  // just past '['
                    array_base_ = base_;
                }
                ++array_depth_;
                break;

            case PdfToken::Type::ArrayEnd:
                if (dict_depth_ > 0) {
                    break;
                }
                if (array_depth_ > 1) {
                    --array_depth_;  // closes a nested array
                } else if (array_depth_ == 1) {
                    array_depth_ = 0;
                    // ']' token position (its 1-char value points
                    // at it).
                    array_end_ = static_cast<std::size_t>(
                        t.value.data() -
                        reinterpret_cast<const char*>(base_));
                    array_pending_ = true;
                } else {
                    ++stats_.stray_array_ends;
                }
                break;

            case PdfToken::Type::DictStart:
                if (array_depth_ > 0) {
                    break;  // array member
                }
                ++dict_depth_;
                break;

            case PdfToken::Type::DictEnd:
                if (array_depth_ > 0) {
                    break;  // array member
                }
                if (dict_depth_ > 0) {
                    --dict_depth_;
                } else {
                    ++stats_.stray_dict_ends;
                }
                break;

            case PdfToken::Type::StreamStart:
            case PdfToken::Type::StreamEnd:
                // A content stream contains no stream objects; a stray
                // keyword pair is tolerated as a separator.
                ++stats_.stray_stream_tokens;
                clear_pending_operands();
                break;
        }
    }
}

}  // namespace pdftoolkit::layout
