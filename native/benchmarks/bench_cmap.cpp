// pdtk_bench_cmap — CMap parse + resolve throughput (audit task 3.1).
//
// Task 3.1's acceptance is ACCURACY (no numeric gate); this benchmark
// exists per the component-exists policy (ADR-0005/P-012 map: a
// benchmark lands with its component) and to give task 3.3 its
// baseline: the per-glyph hot path there is CMapTable::lookup.
//
// Workloads:
//   * ParseCid    — a realistic 2-byte CID-keyed /ToUnicode program
//                   (bfrange blocks + bfchar tail, ~2k entries) parsed
//                   from raw text through the production parse path;
//   * ParseTtf    — a 1-byte TrueType-subset program (sparse bfchar);
//   * Lookup2B / Lookup1B — frozen-table O(1) lookups over a random
//                   code mix (the task-3.3 hot path);
//   * InternMiss / InternHit — the cache's payload-hash + exact-byte
//                   confirmation path, vs the full re-parse a miss
//                   pays. No Flate scenario here: decompression
//                   throughput is pdtk_bench_flate's subject, and
//                   benching it here would drag libdeflate includes
//                   into another TU (P-022 rule) for no new signal.
//
// Per U-012, ParseCid/ParseTtf report a same-run traversal control
// (byte-sum over the same buffer) so the parse/control ratio is
// comparable across environments.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pdftoolkit/font/cmap.hpp"

namespace {

using pdftoolkit::font::CMapCache;
using pdftoolkit::font::CMapTable;

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

std::vector<std::uint8_t> bytes_of(std::string_view text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::string hex4(std::uint32_t v) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04X", v);
    return buf;
}

// A realistic 2-byte CID-keyed /ToUnicode program: contiguous CJK-ish
// bfranges (the 3-operand form producers actually emit), one array
// form, sparse bfchars — ~2k mapped codes, ~60 KB of CMap text.
std::string make_cid_cmap(std::size_t target_entries) {
    Rng rng(11);
    std::string out;
    out += "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n";
    out += "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) "
           "/Supplement 0 >> def\n";
    out += "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n";
    out += "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
    std::uint32_t code = 0x0021;
    std::size_t emitted = 0;
    while (emitted < target_entries) {
        const std::uint32_t width = 16 + rng.below(48);
        if (emitted + width > target_entries) {
            break;
        }
        out += std::to_string(width) + " beginbfrange\n<" + hex4(code) +
               "> <" + hex4(code + width - 1) + "> <" +
               hex4(0x4E00 + rng.below(0x1000)) + ">\nendbfrange\n";
        code += width + rng.below(4);
        emitted += width;
    }
    out += "4 beginbfrange\n<" + hex4(code) + "> <" + hex4(code + 3) +
           "> [<D83DDE00> <FB01> <0178> <00E9>]\nendbfrange\n";
    out += "3 beginbfchar\n<" + hex4(code + 8) + "> <0041>\n<" +
           hex4(code + 9) + "> <0042>\n<" + hex4(code + 10) +
           "> <0043>\nendbfchar\n";
    out += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
    return out;
}

// A 1-byte TrueType-subset program: sparse bfchars.
std::string make_ttf_cmap(std::size_t target_entries) {
    Rng rng(23);
    std::string out;
    out += "/CIDInit /ProcSet findresource begin\nbegincmap\n";
    out += "1 begincodespacerange\n<00> <FF>\nendcodespacerange\n";
    out += std::to_string(target_entries) + " beginbfchar\n";
    for (std::size_t i = 0; i < target_entries; ++i) {
        const std::uint32_t c = 0x20 + static_cast<std::uint32_t>(i % 0x7F);
        char row[64];
        std::snprintf(row, sizeof(row), "<%02X> <%04X>\n", c,
                      0x0020 + rng.below(0x2000));
        out += row;
    }
    out += "endbfchar\nendcmap\n";
    return out;
}

std::vector<std::uint8_t> make_object(std::string_view dict,
                                      std::span<const std::uint8_t> payload) {
    std::string head = "7 0 obj\n<<" + std::string(dict) + ">>\nstream\n";
    std::vector<std::uint8_t> out = bytes_of(head);
    out.insert(out.end(), payload.begin(), payload.end());
    out.insert(out.end(), {'\n', 'e', 'n', 'd', 's', 't', 'r', 'e', 'a',
                           'm', '\n', 'e', 'n', 'd', 'o', 'b', 'j', '\n'});
    return out;
}

// The U-012 same-run control: plain byte traversal of the same buffer.
void traversal_control(benchmark::State& state,
                       const std::vector<std::uint8_t>& buf) {
    std::uint64_t sink = 0;
    for (auto _ : state) {
        std::uint64_t sum = 0;
        for (const std::uint8_t b : buf) {
            sum += b;
        }
        sink = sum;
    }
    benchmark::DoNotOptimize(sink);
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(buf.size()));
    state.SetLabel("control: bytes/s");
}

void bench_parse_cid(benchmark::State& state) {
    const std::string text = make_cid_cmap(
        static_cast<std::size_t>(state.range(0)));
    const std::vector<std::uint8_t> buf = bytes_of(text);
    std::size_t mapped = 0;
    for (auto _ : state) {
        const auto table = CMapTable::parse(buf);
        mapped = table->mapped_codes();
        benchmark::DoNotOptimize(table.get());
    }
    if (mapped == 0) {
        state.SkipWithError("cmap parsed to zero mappings");
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(buf.size()));
    state.SetLabel("CMap text MB/s (CID, " + std::to_string(mapped) +
                   " codes)");
}

void bench_parse_ttf(benchmark::State& state) {
    const std::string text = make_ttf_cmap(
        static_cast<std::size_t>(state.range(0)));
    const std::vector<std::uint8_t> buf = bytes_of(text);
    std::size_t mapped = 0;
    for (auto _ : state) {
        const auto table = CMapTable::parse(buf);
        mapped = table->mapped_codes();
        benchmark::DoNotOptimize(table.get());
    }
    if (mapped == 0) {
        state.SkipWithError("cmap parsed to zero mappings");
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(buf.size()));
    state.SetLabel("CMap text MB/s (TTF subset, " + std::to_string(mapped) +
                   " codes)");
}

template <unsigned CodeBytes>
void bench_lookup(benchmark::State& state) {
    const std::size_t entries = static_cast<std::size_t>(state.range(0));
    std::string text = (CodeBytes == 2 ? make_cid_cmap(entries)
                                       : make_ttf_cmap(entries));
    const auto table = CMapTable::parse(bytes_of(text));
    if (table->mapped_codes() == 0) {
        state.SkipWithError("cmap parsed to zero mappings");
    }

    // Random code mix: half mapped, half probes outside — the shape a
    // content stream produces.
    Rng rng(77);
    std::vector<std::uint32_t> codes(4096);
    for (auto& c : codes) {
        c = CodeBytes == 2 ? rng.below(0x10000) : rng.below(0x100);
    }

    std::size_t hits = 0;
    std::uint64_t sink = 0;
    for (auto _ : state) {
        hits = 0;
        for (const std::uint32_t c : codes) {
            const std::span<const char32_t> seq = table->lookup(c);
            hits += seq.empty() ? 0 : 1;
            sink += static_cast<std::uint64_t>(seq.size());
        }
        benchmark::DoNotOptimize(sink);
        benchmark::DoNotOptimize(hits);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(codes.size()));
    state.SetLabel(std::string(CodeBytes == 2 ? "2" : "1") +
                   "-byte lookups/s (" + std::to_string(hits * 100 /
                   codes.size()) + "% hit)");
}

void bench_intern_miss(benchmark::State& state) {
    // Each iteration interns a DISTINCT payload: full cost — stream
    // anatomy + xxHash64 + parse + table build (raw, no Flate).
    std::uint64_t counter = 0;
    std::size_t size = 0;
    for (auto _ : state) {
        // Vary the CMap so no cache hit can occur.
        std::string text = make_cid_cmap(512);
        char tag[32];
        std::snprintf(tag, sizeof(tag), "x%llu",
                      static_cast<unsigned long long>(counter++));
        text += tag;  // unique payload suffix (after endcmap: ignored)
        const auto object = make_object("", bytes_of(text));
        CMapCache cache;
        const auto table = cache.intern_stream_object(object);
        benchmark::DoNotOptimize(table.get());
        size = cache.size();
    }
    if (size != 1) {
        state.SkipWithError("intern miss did not populate the cache");
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    state.SetLabel("distinct-payload interns/s");
}

void bench_intern_hit(benchmark::State& state) {
    const auto object = make_object(
        "", bytes_of(make_cid_cmap(static_cast<std::size_t>(
                        state.range(0)))));
    CMapCache cache;
    const auto first = cache.intern_stream_object(object);
    benchmark::DoNotOptimize(first.get());
    if (cache.size() != 1) {
        state.SkipWithError("setup intern failed");
    }
    std::size_t hits = 0;
    for (auto _ : state) {
        const auto table = cache.intern_stream_object(object);
        benchmark::DoNotOptimize(table.get());
        ++hits;
    }
    if (cache.intern_hits() != hits) {
        state.SkipWithError("hit accounting mismatch");
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(hits));
    state.SetLabel("repeat-payload interns/s (hash + exact compare)");
}

BENCHMARK(bench_parse_cid)->Arg(2000)->Repetitions(10)->Unit(benchmark::kMicrosecond);
BENCHMARK(bench_parse_ttf)->Arg(200)->Repetitions(10)->Unit(benchmark::kMicrosecond);
BENCHMARK(bench_lookup<2>)->Arg(2000)->Repetitions(10)->Unit(benchmark::kMicrosecond);
BENCHMARK(bench_lookup<1>)->Arg(200)->Repetitions(10)->Unit(benchmark::kMicrosecond);
BENCHMARK(bench_intern_miss)->Repetitions(10)->Unit(benchmark::kMicrosecond);
BENCHMARK(bench_intern_hit)->Arg(2000)->Repetitions(10)->Unit(benchmark::kMicrosecond);

// The control must run in the same process (U-012: never ratio against
// a control from another run).
void bench_control(benchmark::State& state) {
    const std::vector<std::uint8_t> buf =
        bytes_of(make_cid_cmap(2000));
    traversal_control(state, buf);
}
BENCHMARK(bench_control)->Repetitions(10)->Unit(benchmark::kMicrosecond);

}  // namespace
BENCHMARK_MAIN();
