// pdtk_bench_flate — hardware-accelerated Flate decompression throughput
// (audit task 2.4).
//
// The audit's acceptance gate: "Decompression benchmark achieves
// >= 800 MB/s per core on compressed streams." The headline metric is
// DECOMPRESSED output MB/s (the ingestion-relevant rate: bytes of stream
// data produced per second); the label also carries the compressed-input
// rate and the compression ratio. Per U-012, the absolute number is
// reported together with a same-run traversal control (byte-sum of the
// same output buffer) so the ratio "codec / environment ceiling" is
// apples-to-apples across machines.
//
// The measured path is the production one: FlateDecompressor::decompress
// into a BumpArena (chunked staging with rewind), arena.reset() between
// iterations, one decompressor instance for the whole run.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pdftoolkit/codec/flate.hpp"
#include "pdftoolkit/memory/arena.hpp"

#include "libdeflate.h"

namespace {

using pdftoolkit::codec::FlateDecompressor;
using pdftoolkit::codec::FlateLimits;
using pdftoolkit::codec::FlateStatus;
using pdftoolkit::memory::BumpArena;

// Deterministic LCG (no external corpus; reproducible numbers).
struct Rng {
    std::uint32_t state;
    explicit Rng(std::uint32_t seed) : state(seed * 1664525u + 1013904223u) {}
    std::uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
};

// A representative page-content stream (same generator family as
// bench_lexer): text operators, graphics state, paths, dicts — the mix a
// real ingest workload sees. Compresses ~3-4x.
std::vector<std::uint8_t> make_content_stream(std::size_t target_bytes) {
    Rng rng(5);
    std::vector<std::uint8_t> out;
    out.reserve(target_bytes + 512);
    while (out.size() < target_bytes) {
        char buf[256];
        int n;
        switch (rng.next() % 5) {
            case 0:
                n = std::snprintf(buf, sizeof(buf),
                                  "BT /F%u %u.%u Tf %u 0 0 %u %u.%u Tm "
                                  "(Page text segment %u) Tj ET\n",
                                  rng.next() % 6, 6 + rng.next() % 18,
                                  rng.next() % 10, 100 + rng.next() % 2000,
                                  100 + rng.next() % 2000, rng.next() % 612,
                                  rng.next() % 792, rng.next() % 100000);
                break;
            case 1:
                n = std::snprintf(buf, sizeof(buf),
                                  "q %u.%u %u.%u %u.%u %u.%u %u %u cm "
                                  "/Im%u Do Q\n",
                                  rng.next() % 2, rng.next() % 10,
                                  rng.next() % 2, rng.next() % 10,
                                  rng.next() % 2, rng.next() % 10,
                                  rng.next() % 2, rng.next() % 10,
                                  rng.next() % 612, rng.next() % 792,
                                  rng.next() % 64);
                break;
            case 2:
                n = std::snprintf(buf, sizeof(buf),
                                  "%u.%u %u.%u m %u.%u %u.%u l S %u.%u w "
                                  "[%u %u] %u d\n",
                                  rng.next() % 612, rng.next() % 10,
                                  rng.next() % 792, rng.next() % 10,
                                  rng.next() % 612, rng.next() % 10,
                                  rng.next() % 792, rng.next() % 10,
                                  1 + rng.next() % 8, rng.next() % 10,
                                  1 + rng.next() % 9, 1 + rng.next() % 9,
                                  rng.next() % 2);
                break;
            case 3:
                n = std::snprintf(buf, sizeof(buf),
                                  "<</Type/Font/Subtype/Type1/BaseFont/"
                                  "Helvetica/Encoding/WinAnsiEncoding "
                                  "ff=%u gg=%u>>\n",
                                  1000000 + rng.next() % 100000,
                                  1000000 + rng.next() % 100000);
                break;
            default:
                n = std::snprintf(buf, sizeof(buf),
                                  "[(In) -250 (line) 120 (array) -60 "
                                  "(text) %u] TJ\n",
                                  rng.next() % 1000);
                break;
        }
        out.insert(out.end(), buf, buf + n);
    }
    out.resize(target_bytes);
    return out;
}

// Highly repetitive payload (structured rows / uniform pages): the
// ~100x-ratio shape where the codec's chunked staging actually retries.
std::vector<std::uint8_t> make_repetitive(std::size_t target_bytes) {
    std::vector<std::uint8_t> out(target_bytes);
    for (std::size_t i = 0; i < target_bytes; ++i) {
        out[i] = static_cast<std::uint8_t>((i / 97) & 0xFF);
    }
    return out;
}

// Incompressible payload (encrypted/embedded-binary-like): stored
// DEFLATE blocks, ratio ~1.0.
std::vector<std::uint8_t> make_random(std::size_t target_bytes) {
    Rng rng(9);
    std::vector<std::uint8_t> out(target_bytes);
    for (std::size_t i = 0; i < target_bytes; ++i) {
        out[i] = static_cast<std::uint8_t>(rng.next());
    }
    return out;
}

std::vector<std::uint8_t> zlib_wrap(const std::vector<std::uint8_t>& plain,
                                    int level) {
    libdeflate_compressor* comp = libdeflate_alloc_compressor(level);
    if (comp == nullptr) {
        std::fprintf(stderr, "compressor allocation failed\n");
        std::exit(1);
    }
    std::vector<std::uint8_t> out(
        libdeflate_zlib_compress_bound(comp, plain.size()));
    const std::size_t n = libdeflate_zlib_compress(
        comp, plain.data(), plain.size(), out.data(), out.size());
    libdeflate_free_compressor(comp);
    if (n == 0) {
        std::fprintf(stderr, "compression failed\n");
        std::exit(1);
    }
    out.resize(n);
    return out;
}

// File-scope sinks: google/benchmark v1.9.5 deprecates DoNotOptimize on
// scalar const references; the established pattern is a sink pointer +
// ClobberMemory.
std::uint64_t g_sink = 0;
const std::uint8_t* g_ptr_sink = nullptr;

void Decompress(benchmark::State& state, const char* name, int level) {
    const std::vector<std::uint8_t> plain =
        make_content_stream(static_cast<std::size_t>(1) << 24);  // 16 MiB
    const std::vector<std::uint8_t> z = zlib_wrap(plain, level);

    FlateDecompressor dec;
    BumpArena arena(32u << 20);  // >= cap for this workload
    std::size_t out_bytes = 0;
    for (auto _ : state) {
        arena.reset();
        const auto r = dec.decompress(
            std::span<const std::uint8_t>(z), arena, FlateLimits{});
        if (r.status != FlateStatus::Ok) {
            state.SkipWithError(r.message);
            return;
        }
        out_bytes = r.output.size();
        g_ptr_sink = r.output.data();
        g_sink += r.output.front() + r.output.back();
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(out_bytes));
    char label[128];
    std::snprintf(label, sizeof(label),
                  "%s: %.1f MiB -> %.1f MiB out (ratio %.1fx)", name,
                  static_cast<double>(z.size()) / (1024.0 * 1024.0),
                  static_cast<double>(out_bytes) / (1024.0 * 1024.0),
                  static_cast<double>(out_bytes) /
                      static_cast<double>(z.size()));
    state.SetLabel(label);
}

void DecompressRepetitive(benchmark::State& state) {
    const std::vector<std::uint8_t> plain =
        make_repetitive(static_cast<std::size_t>(1) << 24);  // 16 MiB
    const std::vector<std::uint8_t> z = zlib_wrap(plain, 6);

    FlateDecompressor dec;
    BumpArena arena(32u << 20);
    std::size_t out_bytes = 0;
    for (auto _ : state) {
        arena.reset();
        const auto r = dec.decompress(
            std::span<const std::uint8_t>(z), arena, FlateLimits{});
        if (r.status != FlateStatus::Ok) {
            state.SkipWithError(r.message);
            return;
        }
        out_bytes = r.output.size();
        g_ptr_sink = r.output.data();
        g_sink += r.output.front() + r.output.back();
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(out_bytes));
    char label[128];
    std::snprintf(label, sizeof(label),
                  "repetitive rows: %.1f MiB -> %.1f MiB out (ratio %.1fx)",
                  static_cast<double>(z.size()) / (1024.0 * 1024.0),
                  static_cast<double>(out_bytes) / (1024.0 * 1024.0),
                  static_cast<double>(out_bytes) /
                      static_cast<double>(z.size()));
    state.SetLabel(label);
}

void DecompressIncompressible(benchmark::State& state) {
    const std::vector<std::uint8_t> plain =
        make_random(static_cast<std::size_t>(1) << 24);  // 16 MiB
    const std::vector<std::uint8_t> z = zlib_wrap(plain, 1);

    FlateDecompressor dec;
    BumpArena arena(32u << 20);
    std::size_t out_bytes = 0;
    for (auto _ : state) {
        arena.reset();
        const auto r = dec.decompress(
            std::span<const std::uint8_t>(z), arena, FlateLimits{});
        if (r.status != FlateStatus::Ok) {
            state.SkipWithError(r.message);
            return;
        }
        out_bytes = r.output.size();
        g_ptr_sink = r.output.data();
        g_sink += r.output.front() + r.output.back();
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(out_bytes));
    char label[128];
    std::snprintf(label, sizeof(label),
                  "incompressible: %.1f MiB -> %.1f MiB out (ratio %.2fx)",
                  static_cast<double>(z.size()) / (1024.0 * 1024.0),
                  static_cast<double>(out_bytes) / (1024.0 * 1024.0),
                  static_cast<double>(out_bytes) /
                      static_cast<double>(z.size()));
    state.SetLabel(label);
}

// Traversal control (U-012 methodology): byte-sum the same 16 MiB output
// buffer, so the ratio "codec throughput / environment ceiling" is
// measured in the same run against the same allocation.
void TraversalControl(benchmark::State& state) {
    const std::vector<std::uint8_t> buf =
        make_content_stream(static_cast<std::size_t>(1) << 24);  // 16 MiB
    std::uint64_t sink = 0;
    for (auto _ : state) {
        std::uint64_t sum = 0;
        for (const std::uint8_t b : buf) {
            sum += b;
        }
        sink += sum;
        benchmark::DoNotOptimize(sink);
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(buf.size()));
    state.SetLabel("control: byte-sum of the same 16 MiB buffer");
}

BENCHMARK(DecompressRepetitive)
    ->Name("FlateDecompressor/repetitive_16MiB")
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(10)
    ->ReportAggregatesOnly(false);

BENCHMARK(DecompressIncompressible)
    ->Name("FlateDecompressor/incompressible_16MiB")
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(10)
    ->ReportAggregatesOnly(false);

BENCHMARK_CAPTURE(Decompress, content_level1, "content stream (level 1)", 1)
    ->Name("FlateDecompressor/content_l1_16MiB")
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(10)
    ->ReportAggregatesOnly(false);

BENCHMARK_CAPTURE(Decompress, content_level6, "content stream (level 6)", 6)
    ->Name("FlateDecompressor/content_l6_16MiB")
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(10)
    ->ReportAggregatesOnly(false);

BENCHMARK_CAPTURE(Decompress, content_level9, "content stream (level 9)", 9)
    ->Name("FlateDecompressor/content_l9_16MiB")
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(10)
    ->ReportAggregatesOnly(false);

BENCHMARK(TraversalControl)
    ->Name("FlateDecompressor/control_traversal_16MiB")
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(10)
    ->ReportAggregatesOnly(false);

}  // namespace

BENCHMARK_MAIN();
