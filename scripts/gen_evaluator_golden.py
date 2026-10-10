#!/usr/bin/env python3
"""Golden-reference generator for the task-3.2 operator evaluator.

The audit's task-3.2 acceptance gate reads: "Graphics and text matrix
calculations match Adobe Acrobat reference output to within +/-0.001
point precision." No Acrobat output exists in this repository (and
none can — see unknowns U-015), so the gate is interpreted the same
way U-013/U-014 interpreted their corpus acceptances: a deterministic
generator IS the reference. This script models ISO 32000-1:2008
clause 9 (text) semantics INDEPENDENTLY of the C++ evaluator and
emits golden vectors that the native test asserts to +/-0.001 pt.

Precision model (matches ADR-0009, must stay in lockstep with
native/src/layout/evaluator.cpp):

  * every numeric operand the content stream carries is parsed as
    IEEE binary32 (the task-2.3 lexer's token value), then promoted
    to binary64 for all algebra — f32() below;
  * matrix composition (Td/Tm/cm pre-multiplication, P = Tlm x CTM)
    and the glyph-displacement accumulator run in binary64 (the
    evaluator's double carry);
  * expected glyph Trm / final matrices stay binary64 — the native
    side rounds its float Mat6 views at the API boundary, so the
    comparison tolerance (1e-3 pt) absorbs exactly one float32
    rounding (<= ~6e-5 pt at page-scale coordinates).

Number literals are emitted with <= 6 significant digits and
magnitudes < 1e5, keeping every decimal safely outside binary32
rounding-midpoint edge cases (a double-rounding discrepancy would
anyway be <= 1 ulp of binary32 ~ 6e-8 relative, far inside the gate).

Widths: the scenarios use the test resolver's documented formulas:
  F1, F2 (1-byte): w0 = 400 + code % 400   (thousandths)
  CID  (2-byte):   w0 = 300 + code % 900
  WIDE (2-byte):   w0 = 500 + code % 300
The C++ test implements the same formulas for its FontMetricsResolver.
Glyph records carry the font name WITHOUT the leading '/' — the
evaluator's Tf operand convention (token text after the slash).

Usage:  scripts/gen_evaluator_golden.py > native/tests/test_evaluator_golden.inc
"""

import struct
import sys

SEED = 0x3D2E1F  # fixed; changing it invalidates every vector below


def f32(x):
    """Round through IEEE binary32 (the lexer's token precision)."""
    return struct.unpack("<f", struct.pack("<f", x))[0]


def mul(a, b):
    """PDF row-vector product: apply `a` first, then `b`."""
    return (
        a[0] * b[0] + a[1] * b[2],
        a[0] * b[1] + a[1] * b[3],
        a[2] * b[0] + a[3] * b[2],
        a[2] * b[1] + a[3] * b[3],
        a[4] * b[0] + a[5] * b[2] + b[4],
        a[4] * b[1] + a[5] * b[3] + b[5],
    )


IDENTITY = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)


class Lcg:
    def __init__(self, state):
        self.state = state & 0xFFFFFFFF

    def next(self):
        self.state = (self.state * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.state >> 13

    def pick(self, values):
        return values[self.next() % len(values)]


def width_for(font, code):
    if font == "/CID":
        return 300 + code % 900
    if font == "/WIDE":
        return 500 + code % 300
    return 400 + code % 400


def code_bytes_for(font):
    return 2 if font in ("/CID", "/WIDE") else 1


def fmt_num(x):
    """<= 6 significant digits, no exponent — binary32-safe literals."""
    if x == int(x) and abs(x) < 1e6:
        return str(int(x))
    s = f"{x:.6g}"
    assert "e" not in s and "E" not in s, s
    return s


class Ref:
    """Independent reference model of the evaluator's semantics.

    Emission and evaluation happen together: every operator method
    appends its content-stream fragment AND updates the model, so the
    stream and the expected values cannot drift apart.
    """

    def __init__(self):
        self.ctm = IDENTITY
        self.tlm = IDENTITY
        self.displacement = 0.0
        self.in_text = False
        self.st = dict(
            Tc=0.0, Tw=0.0, Tz=100.0, TL=0.0, Tfs=0.0, Ts=0.0, font=""
        )
        self.gstack = []
        self.glyphs = []  # (code, font-no-slash, trm[6], advance)
        self.out = []  # content-stream fragments (joined with spaces)
        self.mark = None  # byte-split offset for the continuation test

    # -- stream ----------------------------------------------------------
    def emit(self, text):
        self.out.append(text)

    def stream(self):
        return " ".join(self.out)

    # -- operators ---------------------------------------------------------
    def set_font(self, font, size):
        self.st["font"] = font
        self.st["Tfs"] = f32(size)
        self.emit(f"{font} {fmt_num(self.st['Tfs'])} Tf")

    def bt(self):
        self.in_text = True
        self.tlm = IDENTITY
        self.displacement = 0.0
        self.emit("BT")

    def et(self):
        self.in_text = False
        self.emit("ET")

    def _apply_td(self, tx, ty):
        self.tlm = mul((1.0, 0.0, 0.0, 1.0, tx, ty), self.tlm)
        self.displacement = 0.0

    def td(self, tx, ty):
        tx, ty = f32(tx), f32(ty)
        self._apply_td(tx, ty)
        self.emit(f"{fmt_num(tx)} {fmt_num(ty)} Td")

    def td_op(self, tx, ty, op):
        assert op in ("Td", "TD")
        tx, ty = f32(tx), f32(ty)
        if op == "TD":
            self.st["TL"] = f32(-ty)
        self._apply_td(tx, ty)
        self.emit(f"{fmt_num(tx)} {fmt_num(ty)} {op}")

    def tstar(self):
        self._apply_td(0.0, f32(-self.st["TL"]))
        self.emit("T*")

    def tm(self, a, b, c, d, e, f):
        m = tuple(f32(v) for v in (a, b, c, d, e, f))
        self.tlm = m
        self.displacement = 0.0
        self.emit(" ".join(fmt_num(v) for v in m) + " Tm")

    def q(self):
        self.gstack.append((self.ctm, dict(self.st)))
        self.emit("q")

    def Q(self):
        if self.gstack:
            self.ctm, self.st = self.gstack.pop()
        self.emit("Q")

    def cm(self, a, b, c, d, e, f):
        m = tuple(f32(v) for v in (a, b, c, d, e, f))
        self.ctm = mul(m, self.ctm)
        self.emit(" ".join(fmt_num(v) for v in m) + " cm")

    def ts(self, key, value, op):
        self.st[key] = f32(value)
        self.emit(f"{fmt_num(self.st[key])} {op}")

    # -- glyph machinery -----------------------------------------------------
    def p_matrix(self):
        return mul(self.tlm, self.ctm)

    def _record_font(self):
        f = self.st["font"]
        return f[1:] if f.startswith("/") else f

    def show_bytes(self, data):
        p = self.p_matrix()
        fs = self.st["Tfs"]
        hz = self.st["Tz"] / 100.0
        tc = self.st["Tc"]
        tw = self.st["Tw"]
        rise = self.st["Ts"]
        sx = fs * hz
        sy = fs
        font = self.st["font"]
        cb = code_bytes_for(font) if font else 1
        i = 0
        while i < len(data):
            if cb == 1:
                code = data[i]
                i += 1
            else:
                if i + 1 >= len(data):
                    break  # odd tail: the reference drops it too
                code = (data[i] << 8) | data[i + 1]
                i += 2
            w0 = width_for(font, code) if font else 1000
            tw_apply = tw if (cb == 1 and code == 32) else 0.0
            tx = w0 / 1000.0 * fs * hz + tc + tw_apply
            trm = (
                sx * p[0],
                sx * p[1],
                sy * p[2],
                sy * p[3],
                self.displacement * p[0] + rise * p[2] + p[4],
                self.displacement * p[1] + rise * p[3] + p[5],
            )
            self.glyphs.append((code, self._record_font(), trm, tx))
            self.displacement += tx

    def show(self, text, quote_op="Tj"):
        """Show a literal string (golden text never contains escapes)."""
        if quote_op == "'":
            self._apply_td(0.0, f32(-self.st["TL"]))
        self.emit(f"({text}) {quote_op}")
        self.show_bytes(text.encode("latin-1"))

    def show_dquote(self, aw, ac, text):
        """The " operator: Tw=aw, Tc=ac, then '."""
        self.st["Tw"] = f32(aw)
        self.st["Tc"] = f32(ac)
        self._apply_td(0.0, f32(-self.st["TL"]))
        self.emit(
            f"{fmt_num(self.st['Tw'])} {fmt_num(self.st['Tc'])}"
            f" ({text}) \""
        )
        self.show_bytes(text.encode("latin-1"))

    def show_hex(self, hexbody):
        self.emit(f"<{hexbody}> Tj")
        digits = "".join(ch for ch in hexbody if ch in "0123456789abcdef")
        if len(digits) % 2:
            digits += "0"
        self.show_bytes(
            bytes(
                int(digits[i : i + 2], 16) for i in range(0, len(digits), 2)
            )
        )

    def tj(self, elements):
        """elements: ('s', text) | ('n', number) — kerning semantics."""
        self.emit(
            "["
            + " ".join(
                f"({v})" if k == "s" else fmt_num(f32(v))
                for k, v in elements
            )
            + "] TJ"
        )
        fs = self.st["Tfs"]
        hz = self.st["Tz"] / 100.0
        for kind, value in elements:
            if kind == "n":
                # ISO 9.4.3: a TJ number's displacement is
                # -(Tj/1000)*Tfs*Th (positive moves LEFT).
                self.displacement -= f32(value) / 1000.0 * fs * hz
            else:
                self.show_bytes(value.encode("latin-1"))

    def graphics_noise(self):
        self.emit(
            "0.5 w [3 2] 0 d 1 0 0 RG 72 700 m 300 700 l S "
            "/GS1 gs /P << /MCID 0 /Toggle true /Ref 4 0 R >> BDC EMC"
        )

    def set_split_here(self):
        self.mark = len(" ".join(self.out))


# --- scenarios ---------------------------------------------------------------


def scenario_plain_tj():
    r = Ref()
    r.bt()
    r.set_font("/F1", 12)
    r.td(100, 700)
    r.show("Hello World")
    r.et()
    return r


def scenario_td_walk():
    r = Ref()
    r.bt()
    r.set_font("/F1", 10)
    r.td(72, 720)
    r.show("line one")
    r.td_op(14, -14, "TD")  # sets TL = 14, then moves
    r.show("line two")
    r.td(0, 0)  # Td(0,0): back to the LINE start (Tj never moved Tlm)
    r.show("overwrite")
    r.tstar()
    r.show("after star")
    r.et()
    return r


def scenario_quote_ops():
    r = Ref()
    r.bt()
    r.set_font("/F1", 11)
    r.td(90, 650)
    r.ts("TL", 13, "TL")
    r.show("first", quote_op="'")
    r.show_dquote(2.5, -1.25, "quoted text")
    r.et()
    return r


def scenario_rotated_tm():
    r = Ref()
    r.bt()
    r.set_font("/F2", 14)
    r.tm(0.866, 0.5, -0.5, 0.866, 72, 720)
    r.show("rotated")
    r.td(18, 0)  # moves along the ROTATED x-axis
    r.show("more")
    r.et()
    return r


def scenario_ctm_stack():
    r = Ref()
    r.cm(2, 0, 0, 2, 100, 50)  # scale 2x, translate
    r.bt()
    r.set_font("/F1", 12)
    r.td(50, 300)
    r.show("scaled")
    r.q()
    r.cm(0.7071, 0.7071, -0.7071, 0.7071, 0, 0)  # rotate 45 degrees
    r.set_font("/F2", 10)
    r.show("rotated too")
    r.Q()  # restores CTM AND the /F1 12 text state
    r.show("back to scale")
    r.et()
    return r


def scenario_kerned_tj():
    r = Ref()
    r.bt()
    r.set_font("/F1", 12)
    r.td(100, 600)
    r.tj([("s", "AV"), ("n", 85), ("s", "I"), ("n", -40), ("s", "AT")])
    r.et()
    return r


def scenario_scaled_rise():
    r = Ref()
    r.bt()
    r.set_font("/F1", 12)
    r.ts("Tz", 50, "Tz")
    r.ts("Ts", 8, "Ts")
    r.ts("Tc", 2, "Tc")
    r.ts("Tw", 4, "Tw")
    r.td(120, 500)
    r.show("squished up")
    r.ts("Tz", 100, "Tz")
    r.show("normal again")
    r.et()
    return r


def scenario_two_fonts_2byte():
    r = Ref()
    r.bt()
    r.set_font("/CID", 12)
    r.td(100, 400)
    data = bytes([0x12, 0x34, 0x00, 0x20, 0xAB, 0xCD, 0x00, 0x01])
    r.emit("(" + "".join(f"\\{b:03o}" for b in data) + ") Tj")
    r.show_bytes(data)
    r.set_font("/F1", 10)
    r.td(0, -20)
    r.show("single byte again")
    r.set_font("/WIDE", 12)
    r.show("wide")
    r.et()
    return r


def scenario_graphics_interleaved():
    r = Ref()
    r.graphics_noise()
    r.bt()
    r.set_font("/F1", 12)
    r.td(72, 700)
    r.show("text one")
    r.graphics_noise()
    r.show("text two")
    r.et()
    r.graphics_noise()
    return r


def scenario_long_line():
    r = Ref()
    r.bt()
    r.set_font("/F1", 10)
    r.td(72, 750)
    rng = Lcg(SEED)
    text = "".join(rng.pick("abcdefghij ") for _ in range(400))
    r.show(text)
    r.et()
    return r


def scenario_hex_strings():
    r = Ref()
    r.bt()
    r.set_font("/F1", 12)
    r.td(80, 650)
    r.show_hex("48656c6c6f")  # Hello
    r.show_hex("41 42")  # A B (whitespace inside)
    r.show_hex("439")  # odd: pads a low 0 -> 0x43 0x90
    r.et()
    return r


def scenario_continuation():
    r = Ref()
    r.bt()
    r.set_font("/F1", 12)
    r.td(100, 700)
    r.show("part one")
    r.set_split_here()
    r.show("part two")
    r.td(0, -14)
    r.show("part three")
    r.et()
    return r


SCENARIOS = [
    ("plain_tj", scenario_plain_tj),
    ("td_walk", scenario_td_walk),
    ("quote_ops", scenario_quote_ops),
    ("rotated_tm", scenario_rotated_tm),
    ("ctm_stack", scenario_ctm_stack),
    ("kerned_tj", scenario_kerned_tj),
    ("scaled_rise", scenario_scaled_rise),
    ("two_fonts_2byte", scenario_two_fonts_2byte),
    ("graphics_interleaved", scenario_graphics_interleaved),
    ("long_line", scenario_long_line),
    ("hex_strings", scenario_hex_strings),
    ("continuation", scenario_continuation),
]


def f32_drift_probe():
    """ADR-0009 evidence: simulate the long_line displacement chain in
    binary32 vs binary64 and report the divergence."""
    rng = Lcg(SEED)
    f32_sum = 0.0
    f64_sum = 0.0
    for _ in range(400):
        code = ord(rng.pick("abcdefghij "))
        w0 = width_for("/F1", code)
        tx = w0 / 1000.0 * 10.0  # Tfs = 10, Tz = 100
        f32_sum = f32(f32_sum + f32(tx))
        f64_sum += tx
    return f32_sum, f64_sum


def c_double(x):
    """Emit a C++ double literal that round-trips exactly."""
    if x == int(x) and abs(x) < 1e15:
        return f"{int(x)}.0"
    return repr(x)


def main():
    glyph_rows = []
    scen_rows = []
    for name, build in SCENARIOS:
        r = build()
        content = r.stream()
        first_glyph = len(glyph_rows)
        for code, font, trm, tx in r.glyphs:
            glyph_rows.append((code, font, trm, tx))
        tm = (
            r.tlm[0],
            r.tlm[1],
            r.tlm[2],
            r.tlm[3],
            r.displacement * r.tlm[0] + r.tlm[4],
            r.displacement * r.tlm[1] + r.tlm[5],
        )
        scen_rows.append(
            (name, content, r.mark or 0, first_glyph, len(r.glyphs),
             r.ctm, r.tlm, tm)
        )

    lines = []
    lines.append(
        "// GENERATED by scripts/gen_evaluator_golden.py — DO NOT EDIT."
    )
    lines.append(
        f"// Seed {SEED:#x}; semantics model: ISO 32000-1:2008 cl. 9"
        " (independent Python reference, U-015)."
    )
    lines.append("// Gate: every glyph Trm field and final matrix within")
    lines.append("// 0.001 pt of the values below (task-3.2 acceptance).")
    lines.append("#pragma once")
    lines.append("")
    lines.append(
        "struct GoldenGlyph { std::uint32_t code; const char* font;"
        " double trm[6]; double advance; };"
    )
    lines.append(
        "struct GoldenScenario { const char* name; const char* content;"
        " std::size_t split; std::size_t first_glyph; std::size_t"
        " glyph_count; double final_ctm[6]; double final_tlm[6];"
        " double final_tm[6]; };"
    )
    lines.append("inline const GoldenGlyph kGoldenGlyphs[] = {")
    for code, font, trm, tx in glyph_rows:
        trm_s = ", ".join(c_double(v) for v in trm)
        lines.append(f"  {{{code}u, \"{font}\", {{{trm_s}}}, {c_double(tx)}}},")
    lines.append("};")
    lines.append("inline const GoldenScenario kGoldenScenarios[] = {")
    for (name, content, split, first, count, ctm, tlm, tm) in scen_rows:
        ctm_s = ", ".join(c_double(v) for v in ctm)
        tlm_s = ", ".join(c_double(v) for v in tlm)
        tm_s = ", ".join(c_double(v) for v in tm)
        esc = (
            content.replace("\\", "\\\\")
            .replace('"', '\\"')
            .replace("\n", "\\n")
        )
        lines.append(
            f'  {{"{name}", "{esc}", {split}, {first}, {count},'
            f" {{{ctm_s}}}, {{{tlm_s}}}, {{{tm_s}}}}},"
        )
    lines.append("};")
    lines.append(
        f"inline constexpr std::size_t kGoldenScenarioCount = {len(scen_rows)};"
    )
    lines.append(
        f"inline constexpr std::size_t kGoldenGlyphCount = {len(glyph_rows)};"
    )
    print("\n".join(lines))

    f32s, f64s = f32_drift_probe()
    print(
        f"[gen_evaluator_golden] f32-accumulation drift probe (long_line,"
        f" 400 glyphs): {f32s!r} vs {f64s!r} -> |drift| ="
        f" {abs(f32s - f64s):.3e} pt (exceeds the 1e-3 gate => ADR-0009"
        f" double carry is load-bearing)",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
