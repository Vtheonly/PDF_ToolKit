// pdtk_bench_evaluator — operator state machine throughput (audit
// task 3.2).
//
// Task 3.2's acceptance is a PRECISION gate (+/-0.001 pt, verified by
// the golden vectors in pdtk_test_evaluator), not a throughput gate;
// this benchmark exists per the component-exists policy (ADR-0005 /
// P-012 map) and to give task 3.3 its baseline: the per-stream cost
// of driving content through the state machine is the front half of
// 3.3's single-pass slab population.
//
// Workloads:
//   * TextHeavy  — realistic BT/Tf/Td/(run)Tj/ET blocks: the glyph
//                  emission path dominates (per glyph: escape-free
//                  decode, advance math, 14-flop Trm, sink call);
//   * KernedTJ   — TJ arrays with a number between every pair of
//                  short strings (array record + re-lex + number
//                  adjustments: the Distiller-style layout mix);
//   * GraphicsHeavy — path/color/dash/marked-content noise with rare
//                  text: measures the tolerant skip path (operands
//                  accumulated and dropped, dicts and arrays skipped);
//   * TraversalControl — byte-sum of the same buffer (U-012 method:
//                  the durable cross-environment number is the
//                  evaluator/control ratio).
//
// The default FontMetricsResolver (constant w0 = 1000, 1-byte codes)
// is used throughout: the benchmark measures the state machine, not
// a consumer's callback table. The sink counts glyphs into a file-
// scope accumulator (google/benchmark's scalar DoNotOptimize is
// deprecated in v1.9.5 — the flate/cmap benches use the same sink
// pattern).

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "pdftoolkit/layout/evaluator.hpp"

namespace {

using pdftoolkit::layout::FontMetricsResolver;
using pdftoolkit::layout::GlyphPlacement;
using pdftoolkit::layout::GlyphSink;
using pdftoolkit::layout::OperatorEvaluator;

// Deterministic LCG (no external corpus; reproducible numbers).
struct Rng {
    std::uint32_t state;
    explicit Rng(std::uint32_t seed) : state(seed * 1664525u + 1013904223u) {}
    std::uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
    std::uint32_t below(std::uint32_t n) { return next() % n; }
};

// File-scope glyph accumulator (the v1.9.5 sink pattern).
std::uint64_t g_glyph_sink = 0;

class CountingSink final : public GlyphSink {
public:
    void on_glyph(const GlyphPlacement& g) noexcept override {
        g_glyph_sink += g.code;
    }
};

std::vector<std::uint8_t> make_text_heavy(std::size_t target_bytes) {
    Rng rng(7);
    std::vector<std::uint8_t> out;
    out.reserve(target_bytes + 512);
    while (out.size() < target_bytes) {
        char buf[512];
        const int n = std::snprintf(
            buf, sizeof(buf),
            "BT /F%u %u.%u Tf %u.%u %u.%u Td 2 Tc "
            "(Lorem ipsum dolor sit amet %u consectetur adipiscing elit "
            "sed do eiusmod tempor %u incididunt ut labore) Tj "
            "0 -%u.%u Td (second line of the paragraph %u ends here) Tj "
            "T* (third line via the star operator %u) Tj ET\n",
            rng.below(6), 6 + rng.below(18), rng.below(10),
            20 + rng.below(580), rng.below(10), 20 + rng.below(740),
            rng.below(10), rng.below(100000), rng.below(100000),
            10 + rng.below(6), rng.below(10), rng.below(100000),
            rng.below(100000));
        out.insert(out.end(), buf, buf + n);
    }
    return out;
}

std::vector<std::uint8_t> make_kerned_tj(std::size_t target_bytes) {
    Rng rng(11);
    std::vector<std::uint8_t> out;
    out.reserve(target_bytes + 512);
    static const char* kWords[] = {"Wa",  "rm",  "nt",  "ke", "rn",  "ed",
                                   "te",  "xt",  "la",  "yo", "An",  "ot",
                                   "he",  "r ",  "li",  "ne"};
    while (out.size() < target_bytes) {
        char buf[512];
        int n = std::snprintf(buf, sizeof(buf),
                              "BT /F%u %u Tf %u %u Td [",
                              rng.below(4), 8 + rng.below(10),
                              20 + rng.below(560), 20 + rng.below(720));
        for (int i = 0; i < 8; ++i) {
            n += std::snprintf(
                buf + n, sizeof(buf) - static_cast<std::size_t>(n),
                "(%s) %d%s", kWords[rng.below(16)],
                (rng.below(2) == 0 ? -1 : 1) *
                    static_cast<int>(10 + rng.below(150)),
                i == 7 ? "" : " ");
        }
        n += std::snprintf(buf + n, sizeof(buf) - static_cast<std::size_t>(n),
                           "] TJ 0 -14 Td [");
        for (int i = 0; i < 8; ++i) {
            n += std::snprintf(
                buf + n, sizeof(buf) - static_cast<std::size_t>(n),
                "(%s) %d%s", kWords[rng.below(16)],
                (rng.below(2) == 0 ? -1 : 1) *
                    static_cast<int>(10 + rng.below(150)),
                i == 7 ? "" : " ");
        }
        n += std::snprintf(buf + n, sizeof(buf) - static_cast<std::size_t>(n),
                           "] TJ ET\n");
        out.insert(out.end(), buf, buf + n);
    }
    return out;
}

std::vector<std::uint8_t> make_graphics_heavy(std::size_t target_bytes) {
    Rng rng(13);
    std::vector<std::uint8_t> out;
    out.reserve(target_bytes + 512);
    while (out.size() < target_bytes) {
        char buf[512];
        int n;
        switch (rng.below(8)) {
            case 0:
            case 1:
            case 2:
                n = std::snprintf(buf, sizeof(buf),
                                  "%u.%u %u.%u m %u.%u %u.%u l %u.%u "
                                  "%u.%u l h S %u.%u w\n",
                                  rng.below(612), rng.below(10),
                                  rng.below(792), rng.below(10),
                                  rng.below(612), rng.below(10),
                                  rng.below(792), rng.below(10),
                                  rng.below(612), rng.below(10),
                                  rng.below(792), rng.below(10),
                                  1 + rng.below(8), rng.below(10));
                break;
            case 3:
            case 4:
                n = std::snprintf(buf, sizeof(buf),
                                  "[%u %u %u %u] %u d %u %u %u RG %u "
                                  "%u %u rg /GS%u gs\n",
                                  rng.below(9), rng.below(9),
                                  rng.below(9), rng.below(9),
                                  rng.below(2), rng.below(256),
                                  rng.below(256), rng.below(256),
                                  rng.below(256), rng.below(256),
                                  rng.below(256), rng.below(8));
                break;
            case 5:
                n = std::snprintf(buf, sizeof(buf),
                                  "/P << /MCID %u /Toggle true /Ref "
                                  "%u 0 R >> BDC /Span <</ActualText "
                                  "(tag %u)>> BDC EMC EMC\n",
                                  rng.below(10000), 1000 + rng.below(4000),
                                  rng.below(10000));
                break;
            case 6:
                n = std::snprintf(
                    buf, sizeof(buf),
                    "q %u.%u %u.%u %u.%u %u.%u %u %u cm /Im%u Do Q\n",
                    rng.below(2), rng.below(10), rng.below(2),
                    rng.below(10), rng.below(2), rng.below(10),
                    rng.below(2), rng.below(10), rng.below(612),
                    rng.below(792), rng.below(64));
                break;
            default:
                n = std::snprintf(buf, sizeof(buf),
                                  "BT /F1 %u Tf %u %u Td (rare text "
                                  "%u) Tj ET\n",
                                  9 + rng.below(6), rng.below(600),
                                  rng.below(760), rng.below(100000));
                break;
        }
        out.insert(out.end(), buf, buf + n);
    }
    return out;
}

// Traversal control (U-012 methodology): byte-sum a representative
// mix with the same allocation/first-touch profile.
void TraversalControl(benchmark::State& state) {
    const std::vector<std::uint8_t> buf =
        make_text_heavy(static_cast<std::size_t>(1) << 24);  // 16 MiB
    std::uint64_t sink = 0;
    for (auto _ : state) {
        std::uint64_t sum = 0;
        for (const std::uint8_t b : buf) {
            sum += b;
        }
        sink += sum;
    }
    g_glyph_sink += sink;
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(buf.size()));
    state.SetLabel("control: byte-sum of the same 16 MiB buffer");
}

template <typename Make>
void RunEvaluator(benchmark::State& state, Make make, const char* label) {
    const std::vector<std::uint8_t> buf = make(static_cast<std::size_t>(1)
                                               << 24);  // 16 MiB
    CountingSink sink;
    std::uint64_t glyphs = 0;
    for (auto _ : state) {
        OperatorEvaluator ev(&sink);
        ev.evaluate(buf);
        glyphs = ev.stats().glyphs_emitted;
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(buf.size()));
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(glyphs));
    char label_buf[128];
    std::snprintf(label_buf, sizeof(label_buf), "%s (%llu glyphs/iter)",
                  label, static_cast<unsigned long long>(glyphs));
    state.SetLabel(label_buf);
    g_glyph_sink += glyphs;
}

void TextHeavy(benchmark::State& state) {
    RunEvaluator(state, make_text_heavy,
                 "BT/Tf/Td/T*/Tj mix - glyph emission path dominates");
}

void KernedTJ(benchmark::State& state) {
    RunEvaluator(state, make_kerned_tj,
                 "TJ arrays - record + replay + number adjustments");
}

void GraphicsHeavy(benchmark::State& state) {
    RunEvaluator(state, make_graphics_heavy,
                 "path/color/dash/BDC noise - tolerant skip path");
}

}  // namespace

BENCHMARK(TraversalControl)
    ->Name("OperatorEvaluator/control_traversal_16MiB")
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(10)
    ->ReportAggregatesOnly(false);

BENCHMARK(TextHeavy)
    ->Name("OperatorEvaluator/text_heavy_16MiB")
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(10)
    ->ReportAggregatesOnly(false);

BENCHMARK(KernedTJ)
    ->Name("OperatorEvaluator/kerned_tj_16MiB")
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(10)
    ->ReportAggregatesOnly(false);

BENCHMARK(GraphicsHeavy)
    ->Name("OperatorEvaluator/graphics_heavy_16MiB")
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(10)
    ->ReportAggregatesOnly(false);

BENCHMARK_MAIN();
