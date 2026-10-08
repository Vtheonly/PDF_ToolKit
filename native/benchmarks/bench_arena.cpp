// pdtk_bench_arena — authoritative BumpArena measurements.
//
// Audit context: task-1.2's acceptance ("1,000,000 slice allocations and a
// .reset() in < 30 microseconds") is physically mis-scaled — 30 us / 1e6 =
// 30 ps per allocation is below any CPU's per-operation latency (problem
// P-011 in docs/recovery/problem-registry.md). The authoritative intent —
// bump allocation is effectively free, reset is O(1) — is what these
// benchmarks measure and what docs/benchmarks/ records.
//
// Every claim citing these numbers must name this binary, the CMake preset
// and the recorded file (AGENTS.md section 5.7).

#include <benchmark/benchmark.h>

#include <cstdint>

#include "pdftoolkit/memory/arena.hpp"

namespace {

using pdftoolkit::memory::BumpArena;

// The audit task-1.2 workload, verbatim: one million allocations of eight
// uint32 (32 bytes each -> 32 MiB of arena traffic) followed by one reset.
// Reported as the total wall time of one workload pass.
void BumpArenaMillionAllocsAndReset(benchmark::State& state) {
    BumpArena arena(1u << 26);  // 64 MiB: the 32 MiB workload plus slack
    for (auto _ : state) {
        for (std::size_t i = 0; i < 1'000'000; ++i) {
            const auto slice = arena.alloc_slice<std::uint32_t>(8);
            benchmark::DoNotOptimize(slice.data());
        }
        arena.reset();
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(
        state.iterations() * static_cast<std::int64_t>(1'000'000));
    state.SetLabel("1M x alloc_slice<uint32[8]> + reset (audit task 1.2)");
}

// Steady-state cost of the arena's core operation pair — one aligned
// allocation plus the O(1) reset — in nanoseconds per pair. This is the
// "allocation is effectively free" evidence at the finest granularity the
// harness can express.
void BumpArenaAllocPlusReset(benchmark::State& state) {
    BumpArena arena(1u << 20);  // 1 MiB is plenty: the cursor never runs far
    for (auto _ : state) {
        const auto slice = arena.alloc_slice<std::uint32_t>(8);
        benchmark::DoNotOptimize(slice.data());
        arena.reset();
        benchmark::ClobberMemory();
    }
    state.SetLabel("alloc_slice<uint32[8]> then reset, per pair");
}

BENCHMARK(BumpArenaMillionAllocsAndReset)
    ->Name("BumpArena/1M_allocs_u32x8_plus_reset")
    ->Unit(benchmark::kMicrosecond)
    // No ReportAggregatesOnly: google/benchmark v1.9.5 has no native
    // percentile output, so the audit's P50/P90/P99 must be computed from
    // the per-repetition samples — which aggregates-only mode discards.
    ->Repetitions(20);

BENCHMARK(BumpArenaAllocPlusReset)
    ->Name("BumpArena/single_alloc_u32x8_plus_reset")
    ->Unit(benchmark::kNanosecond);

}  // namespace

BENCHMARK_MAIN();
