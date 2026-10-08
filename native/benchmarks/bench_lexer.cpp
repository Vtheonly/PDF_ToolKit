// pdtk_bench_lexer — zero-copy tokenizer throughput (audit task 2.3).
//
// The audit's acceptance gate: "Tokenization throughput exceeds
// 2.5 GB/s on uncompressed streams." Per U-012, this reference
// environment's traversal ceiling is ~2.4-2.9 GB/s (even a plain
// vectorized heap byte-sum), so the absolute gate is reported together
// with the ratio to a same-buffer traversal control. The durable,
// cross-environment statement is the ratio (and bytes/token-cycle
// cost), recorded in docs/benchmarks/.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "pdftoolkit/parser/lexer.hpp"

namespace {

using pdftoolkit::parser::PdfToken;
using pdftoolkit::parser::ZeroCopyLexer;

// Deterministic LCG (no external corpus; reproducible numbers).
struct Rng {
    std::uint32_t state;
    explicit Rng(std::uint32_t seed) : state(seed * 1664525u + 1013904223u) {}
    std::uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
};

// A representative page-content stream: text operators, graphics state,
// paths, arrays, dicts and strings — the mix a real ingest workload
// sees on uncompressed content.
std::vector<std::uint8_t> make_content_stream(std::size_t target_bytes) {
    Rng rng(7);
    std::vector<std::uint8_t> out;
    out.reserve(target_bytes + 512);
    while (out.size() < target_bytes) {
        char buf[256];
        int n;
        switch (rng.next() % 6) {
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
            case 4:
                n = std::snprintf(buf, sizeof(buf),
                                  "[(In) -250 (line) 120 (array) -60 "
                                  "(text) %u] TJ\n",
                                  rng.next() % 1000);
                break;
            default:
                n = std::snprintf(buf, sizeof(buf),
                                  "%% a production comment line\n%u %u R "
                                  "endobj %u obj\n",
                                  1000 + rng.next() % 4000,
                                  1000 + rng.next() % 4000, rng.next() % 40);
                break;
        }
        out.insert(out.end(), buf, buf + n);
    }
    return out;
}

// String-heavy workload: the literal-string scanner (escapes, balanced
// parens) dominates.
std::vector<std::uint8_t> make_string_heavy(std::size_t target_bytes) {
    Rng rng(11);
    std::vector<std::uint8_t> out;
    out.reserve(target_bytes + 512);
    while (out.size() < target_bytes) {
        char buf[256];
        int n = std::snprintf(buf, sizeof(buf),
                              "(a (nested \\) string) with \\escapes %u) "
                              "Tj (short%u) '\n",
                              rng.next() % 10000, rng.next() % 100);
        out.insert(out.end(), buf, buf + n);
    }
    return out;
}

// Traversal control (U-012 methodology): byte-sum the same buffer with
// the same allocation/first-touch profile, so the ratio
// "lexer throughput / environment traversal ceiling" is apples-to-apples.
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

void LexContentStream(benchmark::State& state) {
    const std::vector<std::uint8_t> buf =
        make_content_stream(static_cast<std::size_t>(1) << 24);  // 16 MiB
    std::uint64_t token_count = 0;
    std::uint64_t kind_sink = 0;
    for (auto _ : state) {
        ZeroCopyLexer lexer(buf);
        std::uint64_t count = 0;
        std::uint64_t kinds = 0;
        PdfToken tok;
        while ((tok = lexer.next()).type != PdfToken::Type::EndOfFile) {
            ++count;
            kinds += static_cast<std::uint64_t>(tok.type);
        }
        token_count = count;
        kind_sink += kinds;
        benchmark::DoNotOptimize(kind_sink);
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(buf.size()));
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(token_count));
    state.SetLabel("content-stream mix (operators, dicts, strings, numbers)");
}

void LexStringHeavy(benchmark::State& state) {
    const std::vector<std::uint8_t> buf =
        make_string_heavy(static_cast<std::size_t>(1) << 24);  // 16 MiB
    std::uint64_t token_count = 0;
    std::uint64_t kind_sink = 0;
    for (auto _ : state) {
        ZeroCopyLexer lexer(buf);
        std::uint64_t count = 0;
        std::uint64_t kinds = 0;
        PdfToken tok;
        while ((tok = lexer.next()).type != PdfToken::Type::EndOfFile) {
            ++count;
            kinds += static_cast<std::uint64_t>(tok.type);
        }
        token_count = count;
        kind_sink += kinds;
        benchmark::DoNotOptimize(kind_sink);
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(buf.size()));
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(token_count));
    state.SetLabel("string-heavy (escapes, balanced parens)");
}

BENCHMARK(TraversalControl)
    ->Name("ZeroCopyLexer/control_traversal_16MiB")
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(10)
    ->ReportAggregatesOnly(false);

BENCHMARK(LexContentStream)
    ->Name("ZeroCopyLexer/content_stream_16MiB")
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(10)
    ->ReportAggregatesOnly(false);

BENCHMARK(LexStringHeavy)
    ->Name("ZeroCopyLexer/string_heavy_16MiB")
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(10)
    ->ReportAggregatesOnly(false);

}  // namespace

BENCHMARK_MAIN();
