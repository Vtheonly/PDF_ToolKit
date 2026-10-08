// pdtk_bench_mmap — authoritative MmapHandle measurements (audit task 1.1
// / task 0.2's bench_mmap.cpp slot: "file read throughput (GB/s)").
//
// What is measured, and what deliberately is NOT:
//
//   * sequential_read — steady-state traversal of a warm, live mapping
//     (the engine's hot loop: bytes are already mapped, page cache is
//     warm). This is the zero-copy traversal ceiling the lexer (task
//     2.3) and scanner (task 4.2) build upon.
//
//   * map_and_first_touch — the full per-document ingestion lifecycle:
//     open + fstat + mmap + first touch of every page (minor faults;
//     page cache warm) + munmap + close. Reported in bytes/s so it is
//     directly comparable to sequential_read; the gap between the two
//     is the per-byte page-fault and syscall overhead.
//
//   * open_map_advise_close — the syscall-path cost alone (no data
//     access): open + fstat + mmap + posix_madvise(WILLNEED) + munmap +
//     close, in nanoseconds per lifecycle.
//
// Cold-disk throughput is NOT measured (and cannot be honestly measured
// on a warm page cache): these numbers isolate the engine from disk
// hardware — see docs/benchmarks/ method notes and U-012.
//
// Every claim citing these numbers must name this binary, the CMake
// preset and the recorded file (AGENTS.md section 5.7).

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#else
#include <io.h>
#endif

#include "pdftoolkit/memory/mmap.hpp"

namespace {

using pdftoolkit::memory::Advice;
using pdftoolkit::memory::MmapHandle;

// RAII corpus file with deterministic LCG content (no <random> weight in
// the benchmark process; content only needs to be non-degenerate).
class CorpusFile {
public:
    explicit CorpusFile(std::size_t bytes) {
        std::string tmpl =
            (std::filesystem::temp_directory_path() / "pdtk_bench_mmap_XXXXXX")
                .string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
#ifdef _WIN32
        throw std::runtime_error("CorpusFile: mkstemp is POSIX-only");
#else
        const int fd = ::mkstemp(buf.data());
#endif
        if (fd < 0) {
            throw std::runtime_error("CorpusFile: mkstemp failed");
        }
        path_ = buf.data();

        constexpr std::size_t kChunk = 64 * 1024;
        std::vector<std::uint8_t> chunk(kChunk);
        std::uint32_t x = 0x6d2b79f5u;
        std::size_t written = 0;
        try {
            while (written < bytes) {
                for (auto& b : chunk) {
                    x = x * 1664525u + 1013904223u;
                    b = static_cast<std::uint8_t>(x >> 24);
                }
                const std::size_t n = std::min(kChunk, bytes - written);
                std::size_t done = 0;
                while (done < n) {
                    const ssize_t m =
                        ::write(fd, chunk.data() + done, n - done);
                    if (m <= 0) {
                        throw std::runtime_error("CorpusFile: write failed");
                    }
                    done += static_cast<std::size_t>(m);
                }
                written += n;
            }
        } catch (...) {
            ::close(fd);
            std::error_code ec;
            std::filesystem::remove(path_, ec);
            throw;
        }
        ::close(fd);
    }

    ~CorpusFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    CorpusFile(const CorpusFile&) = delete;
    CorpusFile& operator=(const CorpusFile&) = delete;

    const char* c_str() const { return path_.c_str(); }

private:
    std::string path_;
};

std::uint64_t accumulate(std::span<const std::uint8_t> bytes) {
    std::uint64_t sum = 0;
    for (const std::uint8_t b : bytes) {
        sum += b;
    }
    return sum;
}

// Escape sink: google/benchmark v1.9.5 deprecates DoNotOptimize for
// small scalars (const-ref overload); passing &sink selects the
// non-deprecated pointer overload, and the escaped address guarantees
// the accumulation cannot be dead-code-eliminated.
std::uint64_t g_sink = 0;

// 128 MiB: large enough that per-iteration fixed costs are amortised at
// GB/s rates, small enough for a two-core CI runner (~0.5 s at 250 MB/s).
constexpr std::size_t kCorpusBytes = std::size_t{128} << 20;

void MmapSequentialRead(benchmark::State& state) {
    static CorpusFile corpus(kCorpusBytes);
    MmapHandle handle(corpus.c_str());
    // Warm the mapping and the page cache outside the timed region: the
    // reported number must be the steady-state traversal cost, not the
    // one-off page-in (that cost is the subject of MapAndFirstTouch).
    g_sink = accumulate(handle.bytes());
    benchmark::DoNotOptimize(&g_sink);

    for (auto _ : state) {
        g_sink = accumulate(handle.bytes());
        benchmark::DoNotOptimize(&g_sink);
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<std::int64_t>(kCorpusBytes));
    state.SetLabel("warm mapping: sequential byte-sum over 128 MiB");
}

void MmapMapAndFirstTouch(benchmark::State& state) {
    static CorpusFile corpus(kCorpusBytes);
    // Pre-warm the page cache only (disk-level cold reads must not skew
    // the first iteration): a throwaway mapping touches every page once.
    {
        MmapHandle warm(corpus.c_str());
        g_sink = accumulate(warm.bytes());
        benchmark::DoNotOptimize(&g_sink);
    }

    for (auto _ : state) {
        MmapHandle handle(corpus.c_str());  // fresh mapping per iteration
        g_sink = accumulate(handle.bytes());
        benchmark::DoNotOptimize(&g_sink);
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<std::int64_t>(kCorpusBytes));
    state.SetLabel("per-pass lifecycle: map + first-touch every page + unmap (page cache warm)");
}

void MmapOpenMapAdviseClose(benchmark::State& state) {
    static CorpusFile corpus(1u << 20);  // 1 MiB: size is irrelevant here
    for (auto _ : state) {
        MmapHandle handle(corpus.c_str());
        benchmark::DoNotOptimize(handle.bytes().data());
        (void)handle.advise(Advice::WillNeed);
    }
    state.SetLabel("open + fstat + mmap + advise(WILLNEED) + munmap + close");
}

BENCHMARK(MmapSequentialRead)
    ->Name("MmapHandle/sequential_read_128MiB")
    ->Unit(benchmark::kMicrosecond)
    // 20 repetitions: the audit's distribution claims (P50/P90/P99) are
    // computed from per-repetition samples — aggregates-only mode would
    // discard them (docs/benchmarks/README.md policy).
    ->Repetitions(20);

BENCHMARK(MmapMapAndFirstTouch)
    ->Name("MmapHandle/map_and_first_touch_128MiB")
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(20);

BENCHMARK(MmapOpenMapAdviseClose)
    ->Name("MmapHandle/open_map_advise_close_1MiB")
    ->Unit(benchmark::kNanosecond);

}  // namespace

BENCHMARK_MAIN();
