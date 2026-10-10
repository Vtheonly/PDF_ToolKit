# ADR-0009: Evaluator exposes float32 matrices, carries double internally

* **Status:** Accepted (2026-10-10, task 3.2)
* **Context:** audit issue-1/task-3.2 mandates "affine matrices in CPU
  registers using `std::array<float, 6>`" AND an acceptance gate of
  "±0.001 pt vs Acrobat reference output". A pure-float32 evaluation
  cannot meet that gate on long text runs: the glyph-displacement
  accumulator and the Tm/Tlm composition chain round every step at
  float32, and the error compounds. Measured on the golden generator's
  400-glyph long-line scenario (seed 0x3D2E1F, Tfs 10): the
  float32-accumulated displacement ends 1986.8413 vs the true
  1986.8400 — a drift of **1.309e-03 pt, already past the 1e-3 gate at
  a mere 400 glyphs**, with realistic pages containing far more.
* **Decision:** the evaluator's PUBLIC surface keeps the audit's
  `Mat6 = std::array<float, 6>` (matrices, glyph events), but its
  INTERNAL carry is double-precision:
  * the glyph displacement D accumulates in `double`;
  * Tlm and CTM compose in `double` (`std::array<double, 6>` mirrors,
    including inside q/Q frames);
  * the per-glyph text rendering matrix is the 14-flop closed form
    Trm = [sx 0 0 sy D Ts] × (Tlm × CTM), all in `double`, rounded to
    float32 exactly once at the API boundary;
  * text-state scalars (Tc/Tw/Tz/TL/Tfs/Ts) remain float32 as the
  tokens delivered them, promoted to double at point of use.
* **Rationale:**
  * The invariant Tm ≡ T_tx(D) × Tlm (translations in one frame
    commute-add exactly) reduces per-glyph matrix churn to ONE scalar
    accumulation — carrying that scalar and the two composed matrices
    in double costs ~nothing (the per-glyph work is 14 FLOPs either
    way) and keeps 400-glyph drift at ~1e-12 pt.
  * Acrobat itself does not evaluate in float32: its fixed-point
    coordinates (ASFixed, 16.16) have ~1.5e-5 pt resolution, tighter
    than float32 at page scale — so "match Acrobat to ±0.001 pt"
    never implied float32 arithmetic was sufficient.
  * The audit's `std::array<float, 6>` language is about storage shape
    and register residency (no heap matrix objects), which the public
    Mat6 keeps; doubles in registers satisfy the same intent.
* **Consequences:**
  * The golden-reference generator (scripts/gen_evaluator_golden.py)
    models exactly this pipeline: binary32 operand parse (the task-2.3
    lexer's token precision), binary64 algebra, golden values left in
    binary64 so the 1e-3 comparison absorbs precisely one float32
    rounding (~6e-5 pt at page scale).
  * The exposed `Mat6` accessors (`ctm()`, `text_line_matrix()`,
    `text_matrix()`) round once per call — tests comparing them use
    1e-3 tolerances, not exact equality.
  * Any future component that must reproduce the evaluator's
    coordinates bit-for-bit (task 3.3's slab writes derive from the
    same glyph events) must consume the GlyphPlacement floats, not
    re-derive positions in a different precision.
  * If a future gate ever demands exact float32 reproducibility across
    implementations, THIS ADR is the thing to revisit — the honest
    alternative would have been failing the ±0.001 pt gate on long
    lines.
