// pdtk_bench_slab — authoritative Unified Page Slab measurements
// (audit task 1.3 / task 0.2's bench_slab.cpp slot: "cache hit rate and
// coordinate extraction latency").
//
// What is measured, and what deliberately is NOT:
//
//   * coordinate_extraction — the audit's named latency: iterating the
//     four SoA coordinate arrays of a live, warm slab (the workload of
//     every geometry/spatial pass: task 4.4). Reported as glyphs/s of
//     coordinate extraction.
//
//   * build — the ingestion-side cost: unboxed glyph source in, slab in
//     the arena (including the zero-fill and the CRC), plus the arena
//     reset, per pass. Reported as glyphs/s built.
//
// "Cache hit rate" (L1-dcache hits) needs PMU counters; perf is not
// available in the reference environment (U-010) and is honestly
// SKIPPED by scripts/run_perf.sh — the counter rows cannot be recorded
// until a self-hosted/privileged runner exists. Absolute rates are
// environment-bound (see U-012 and the phase-1 mmap baseline's control
// experiment); the durable statements are the per-glyph costs and the
// build/extract ratio.
//
// Every claim citing these numbers must name this binary, the CMake
// preset and the recorded file (AGENTS.md section 5.7).

#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

#include "pdftoolkit/memory/arena.hpp"
#include "pdftoolkit/memory/page_slab.hpp"

namespace {

using pdftoolkit::memory::BumpArena;
using pdftoolkit::memory::PageSlabSource;
using pdftoolkit::memory::PageSlabView;
using pdftoolkit::memory::build_page_slab;

// Escape sink (google/benchmark v1.9.5 deprecates DoNotOptimize for
// small scalars; &sink selects the non-deprecated pointer overload and
// the escaped address blocks dead-code elimination of the accumulators).
float g_sink_x = 0.0F;
float g_sink_y = 0.0F;
float g_sink_w = 0.0F;
float g_sink_h = 0.0F;

// One pass over all four coordinate arrays — the "coordinate
// extraction" workload. Kept out-of-line-equivalent to what a real
// geometry pass does: sequential AVX2-friendly float loads.
void extract_coordinates(const PageSlabView& view) {
    float sx = 0.0F, sy = 0.0F, sw = 0.0F, sh = 0.0F;
    const std::span<const float> x = view.x();
    const std::span<const float> y = view.y();
    const std::span<const float> w = view.w();
    const std::span<const float> h = view.h();
    for (std::size_t i = 0; i < x.size(); ++i) {
        sx += x[i];
        sy += y[i];
        sw += w[i];
        sh += h[i];
    }
    g_sink_x = sx;
    g_sink_y = sy;
    g_sink_w = sw;
    g_sink_h = sh;
}

// Deterministic glyph source (same LCG family as the tests).
struct GlyphSource {
    std::vector<float> x, y, w, h;
    std::vector<std::uint32_t> codepoints;
    std::vector<std::uint64_t> term_hashes;
    std::vector<std::uint8_t> postings;

    PageSlabSource source() const {
        return PageSlabSource{1u,        595.0F, 842.0F,
                              x,         y,      w,
                              h,         codepoints,
                              term_hashes, postings};
    }

    std::size_t glyph_count() const { return x.size(); }
};

GlyphSource make_source(std::size_t glyphs, std::size_t terms) {
    GlyphSource src;
    src.x.resize(glyphs);
    src.y.resize(glyphs);
    src.w.resize(glyphs);
    src.h.resize(glyphs);
    src.codepoints.resize(glyphs);
    std::uint32_t state = 0xB5297A4Du;
    const auto next = [&state] {
        state = state * 1664525u + 1013904223u;
        return state;
    };
    for (std::size_t i = 0; i < glyphs; ++i) {
        src.x[i] = static_cast<float>(next() % 65536);
        src.y[i] = static_cast<float>(next() % 65536);
        src.w[i] = static_cast<float>(next() % 1024) * 0.5F;
        src.h[i] = static_cast<float>(next() % 1024) * 0.25F;
        src.codepoints[i] = next();
    }
    src.term_hashes.resize(terms);
    std::uint64_t hash = 0x123456789ABCDEF0ull;
    for (std::size_t i = 0; i < terms; ++i) {
        src.term_hashes[i] = hash;
        hash += 0x0000000164557ABFull;  // wrap-safe: stays sorted
    }
    src.postings.resize(terms * 16u);
    for (std::size_t i = 0; i < src.postings.size(); ++i) {
        src.postings[i] = static_cast<std::uint8_t>(next());
    }
    return src;
}

// 1M glyphs: the audit's "query execution over 10,000 pages" scale is
// ~100-200 glyphs/page; 1M glyphs is one heavy page-side pass or a
// mid-size page batch, and keeps the slab (~20 MB) well inside cache
// pressure of a two-core runner.
constexpr std::size_t kGlyphs = std::size_t{1} << 20;
constexpr std::size_t kTerms = 4096;

void PageSlabCoordinateExtraction(benchmark::State& state) {
    static const GlyphSource src = make_source(kGlyphs, kTerms);
    BumpArena arena(kGlyphs * 24 + (1u << 16));  // slab ~20 MB + slack
    const auto slab = build_page_slab(arena, src.source());
    if (slab.empty()) {
        state.SkipWithError("arena too small for the slab");
        return;
    }
    const PageSlabView view(slab);
    extract_coordinates(view);  // warm

    for (auto _ : state) {
        extract_coordinates(view);
    }
    benchmark::DoNotOptimize(&g_sink_x);
    state.SetItemsProcessed(state.iterations() *
                            static_cast<std::int64_t>(view.glyph_count()));
    state.SetLabel("sum x/y/w/h over 1M-glyph SoA arrays (warm, 32B-aligned)");
}

void PageSlabBuild(benchmark::State& state) {
    static const GlyphSource src = make_source(kGlyphs, kTerms);
    BumpArena arena(kGlyphs * 24 + (1u << 16));

    for (auto _ : state) {
        const auto slab = build_page_slab(arena, src.source());
        benchmark::DoNotOptimize(slab.data());
        arena.reset();  // per-pass lifecycle: build + reset
    }
    state.SetItemsProcessed(state.iterations() *
                            static_cast<std::int64_t>(kGlyphs));
    state.SetLabel("unbox 1M glyphs into a slab (memcpy + zero-fill + CRC) + reset");
}

BENCHMARK(PageSlabCoordinateExtraction)
    ->Name("PageSlab/coordinate_extraction_1M_glyphs")
    ->Unit(benchmark::kMillisecond)
    // 20 repetitions: distribution claims (P50/P90/P99) are computed
    // from per-repetition samples — aggregates-only mode discards them
    // (docs/benchmarks/README.md policy).
    ->Repetitions(20);

BENCHMARK(PageSlabBuild)
    ->Name("PageSlab/build_1M_glyphs")
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(20);

}  // namespace

BENCHMARK_MAIN();
