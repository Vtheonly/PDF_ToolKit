// Flate codec tests (audit issue-1/task-2.4).
//
// Coverage contract:
//   * round-trips through libdeflate's compressor (the suite builds its
//     own zlib-wrapped fixtures — no binary fixtures on disk, per the
//     repo test policy), from empty payload to 1 MiB mixed content;
//   * arena integration: output lands inside the caller's BumpArena,
//     chunk growth retries REUSE the failed attempt's bytes (rewind),
//     reset() + re-decompress works;
//   * decompression-bomb defenses (audit step 4): expansion-ratio cap,
//     absolute output cap, trusted size hint as a hard bound, zero
//     budget, arena exhaustion — every refusal leaves the arena at its
//     mark;
//   * error mapping: corrupt payload, truncated payload, gzip-vs-zlib
//     format confusion -> BadData;
//   * determinism: 200 hostile random buffers, decompressed twice.
//
// With PDTK_ENABLE_FLATE=OFF (offline profile) the stub semantics are
// asserted instead: supported() == false, decompress -> Unavailable.

#include "test_harness.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "pdftoolkit/codec/flate.hpp"
#include "pdftoolkit/memory/arena.hpp"

#ifndef PDTK_HAVE_FLATE
#define PDTK_HAVE_FLATE 0
#endif

namespace {

using pdftoolkit::codec::FlateDecompressor;
using pdftoolkit::codec::FlateLimits;
using pdftoolkit::codec::FlateResult;
using pdftoolkit::codec::FlateStatus;
using pdftoolkit::memory::BumpArena;

struct Rng {
    std::uint32_t state;
    explicit Rng(std::uint32_t seed) : state(seed * 1664525u + 1013904223u) {}
    std::uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
    std::uint32_t below(std::uint32_t n) { return next() % n; }
};

#if PDTK_HAVE_FLATE

#include "libdeflate.h"

// --- fixtures ----------------------------------------------------------------

// Compresses `plain` into a zlib wrapper (the PDF /FlateDecode format).
std::vector<std::uint8_t> zlib_wrap(const std::vector<std::uint8_t>& plain,
                                    int level = 6) {
    libdeflate_compressor* comp = libdeflate_alloc_compressor(level);
    PDTK_ASSERT(comp != nullptr);
    std::vector<std::uint8_t> out(
        libdeflate_zlib_compress_bound(comp, plain.size()));
    const std::size_t n = libdeflate_zlib_compress(
        comp, plain.data(), plain.size(), out.data(), out.size());
    libdeflate_free_compressor(comp);
    PDTK_ASSERT(n > 0);
    out.resize(n);
    return out;
}

// Gzip wrapper (RFC 1952) — a DIFFERENT container that must be refused.
std::vector<std::uint8_t> gzip_wrap(const std::vector<std::uint8_t>& plain) {
    libdeflate_compressor* comp = libdeflate_alloc_compressor(6);
    PDTK_ASSERT(comp != nullptr);
    std::vector<std::uint8_t> out(
        libdeflate_gzip_compress_bound(comp, plain.size()));
    const std::size_t n = libdeflate_gzip_compress(
        comp, plain.data(), plain.size(), out.data(), out.size());
    libdeflate_free_compressor(comp);
    PDTK_ASSERT(n > 0);
    out.resize(n);
    return out;
}

// Mixed content: a page-content-stream-like mix (compressible text with
// numeric and structural noise — the workload PDF ingestion sees).
std::vector<std::uint8_t> make_mixed(std::size_t target_bytes,
                                     std::uint32_t seed) {
    Rng rng(seed);
    std::vector<std::uint8_t> out;
    out.reserve(target_bytes + 256);
    while (out.size() < target_bytes) {
        char buf[192];
        int n;
        switch (rng.below(4)) {
            case 0:
                n = std::snprintf(buf, sizeof(buf),
                                  "BT /F%u %u Tf %u.%u %u.%u Td "
                                  "(segment %u) Tj ET\n",
                                  rng.below(6), 8 + rng.below(12),
                                  rng.below(612), rng.below(10),
                                  rng.below(792), rng.below(10),
                                  rng.below(100000));
                break;
            case 1:
                n = std::snprintf(buf, sizeof(buf),
                                  "%u.%u %u.%u m %u.%u %u.%u l S\n",
                                  rng.below(612), rng.below(10),
                                  rng.below(792), rng.below(10),
                                  rng.below(612), rng.below(10),
                                  rng.below(792), rng.below(10));
                break;
            case 2:
                n = std::snprintf(buf, sizeof(buf),
                                  "<</Type/Page/MediaBox[0 0 %u %u]>>\n",
                                  rng.below(612), rng.below(792));
                break;
            default:
                n = std::snprintf(buf, sizeof(buf),
                                  "0x%02X%02X%02X%02X hexdata %u\n",
                                  rng.below(256), rng.below(256),
                                  rng.below(256), rng.below(256),
                                  rng.below(10000));
                break;
        }
        out.insert(out.end(), buf, buf + n);
    }
    out.resize(target_bytes);
    return out;
}

FlateResult run(FlateDecompressor& dec, const std::vector<std::uint8_t>& in,
                BumpArena& arena, const FlateLimits& limits) {
    return dec.decompress(std::span<const std::uint8_t>(in), arena, limits);
}

// --- codec unit cases ----------------------------------------------------------

PDTK_TEST(supported_reports_true) {
    PDTK_ASSERT(FlateDecompressor::supported());
}

PDTK_TEST(round_trip_text) {
    const std::vector<std::uint8_t> plain = [] {
        const char* s = "Hello, PDF FlateDecode world!";
        return std::vector<std::uint8_t>(s, s + std::strlen(s));
    }();
    const std::vector<std::uint8_t> z = zlib_wrap(plain);
    FlateDecompressor dec;
    BumpArena arena(1u << 20);
    const FlateResult r = run(dec, z, arena, FlateLimits{});
    PDTK_ASSERT_EQ(static_cast<int>(r.status), static_cast<int>(FlateStatus::Ok));
    PDTK_ASSERT_EQ(r.output.size(), plain.size());
    PDTK_ASSERT(std::memcmp(r.output.data(), plain.data(), plain.size()) == 0);
}

PDTK_TEST(round_trip_empty_payload) {
    // A zlib stream of an empty payload is legal (~8 bytes) and must
    // decompress to an empty span, not an error.
    const std::vector<std::uint8_t> z = zlib_wrap({});
    FlateDecompressor dec;
    BumpArena arena(4096);
    const FlateResult r = run(dec, z, arena, FlateLimits{});
    PDTK_ASSERT_EQ(static_cast<int>(r.status), static_cast<int>(FlateStatus::Ok));
    PDTK_ASSERT_EQ(r.output.size(), std::size_t{0});
}

PDTK_TEST(round_trip_size_sweep) {
    FlateDecompressor dec;
    BumpArena arena(8u << 20);
    for (const std::size_t size : {std::size_t{1}, std::size_t{100},
                                   std::size_t{4096}, std::size_t{64} << 10,
                                   std::size_t{256} << 10,
                                   std::size_t{1} << 20}) {
        const std::vector<std::uint8_t> plain = make_mixed(size, 991);
        const std::vector<std::uint8_t> z = zlib_wrap(plain);
        arena.reset();
        const FlateResult r = run(dec, z, arena, FlateLimits{});
        PDTK_ASSERT_EQ(static_cast<int>(r.status),
                       static_cast<int>(FlateStatus::Ok));
        PDTK_ASSERT_EQ(r.output.size(), plain.size());
        PDTK_ASSERT(std::memcmp(r.output.data(), plain.data(),
                                plain.size()) == 0);
    }
}

PDTK_TEST(output_lands_inside_the_arena) {
    const std::vector<std::uint8_t> plain = make_mixed(100'000, 7);
    const std::vector<std::uint8_t> z = zlib_wrap(plain);
    FlateDecompressor dec;
    BumpArena arena(1u << 20);
    const std::size_t mark = arena.used_bytes();
    const FlateResult r = run(dec, z, arena, FlateLimits{});
    PDTK_ASSERT_EQ(static_cast<int>(r.status), static_cast<int>(FlateStatus::Ok));
    PDTK_ASSERT(arena.owns(r.output.data()));
    PDTK_ASSERT(arena.used_bytes() >= mark + r.output.size());

    // reset() reclaims the staging bytes; a second run works.
    arena.reset();
    const FlateResult r2 = run(dec, z, arena, FlateLimits{});
    PDTK_ASSERT_EQ(static_cast<int>(r2.status), static_cast<int>(FlateStatus::Ok));
    PDTK_ASSERT_EQ(r2.output.size(), plain.size());
}

PDTK_TEST(chunk_growth_reuses_failed_attempts) {
    // A payload whose compression ratio sits strictly between the
    // initial-guess factor (4x compressed) and this test's permissive
    // ratio cap: the first speculative chunk is necessarily too small,
    // and the codec must retry with doubled chunks. The retries rewind
    // the arena, so consumption stays at the FINAL chunk, not the sum
    // of all attempts. Built by tiling a 16 KiB mixed block (period is
    // within DEFLATE's 32 KiB match window, so cross-block matches
    // fire and the ratio lands well below 1024x).
    constexpr std::size_t kPlain = 5u << 20;  // 5 MiB
    const std::vector<std::uint8_t> unit = make_mixed(16u << 10, 123);
    std::vector<std::uint8_t> plain;
    plain.reserve(kPlain);
    while (plain.size() < kPlain) {
        plain.insert(plain.end(), unit.begin(), unit.end());
    }
    plain.resize(kPlain);
    const std::vector<std::uint8_t> z = zlib_wrap(plain);
    // The initial guess (8x compressed) must sit below the output so
    // growth actually happens.
    PDTK_ASSERT(z.size() < kPlain / 8);
    FlateDecompressor dec;
    BumpArena arena(16u << 20);
    const std::size_t mark = arena.used_bytes();
    // This test exercises the staging mechanics, not the defense caps
    // (those have their own tests): a permissive ratio keeps the tiled
    // payload comfortably legal while the doubling retries do the work.
    FlateLimits limits;
    limits.expansion_ratio = 1024;
    const FlateResult r = run(dec, z, arena, limits);
    PDTK_ASSERT_EQ(static_cast<int>(r.status), static_cast<int>(FlateStatus::Ok));
    PDTK_ASSERT_EQ(r.output.size(), kPlain);
    const std::size_t consumed = arena.used_bytes() - mark;
    // No accumulation: at most one final chunk (plus doubling slack of
    // less than 2x) — NOT the multi-attempt sum (which would exceed
    // the sum of every doubled chunk).
    PDTK_ASSERT(consumed < 2 * kPlain);
    PDTK_ASSERT(consumed >= kPlain);
    PDTK_ASSERT(std::memcmp(r.output.data(), plain.data(), kPlain) == 0);
}

PDTK_TEST(corrupt_payload_is_bad_data) {
    std::vector<std::uint8_t> z = zlib_wrap(make_mixed(200'000, 21));
    z[z.size() / 2] ^= 0xFF;  // flip a mid-stream byte
    FlateDecompressor dec;
    BumpArena arena(1u << 20);
    const FlateResult r = run(dec, z, arena, FlateLimits{});
    PDTK_ASSERT_EQ(static_cast<int>(r.status),
                   static_cast<int>(FlateStatus::BadData));
}

PDTK_TEST(truncated_payload_is_bad_data) {
    const std::vector<std::uint8_t> z = zlib_wrap(make_mixed(200'000, 22));
    const std::vector<std::uint8_t> cut(z.begin(), z.begin() + z.size() * 3 / 5);
    FlateDecompressor dec;
    BumpArena arena(1u << 20);
    const FlateResult r = run(dec, cut, arena, FlateLimits{});
    PDTK_ASSERT_EQ(static_cast<int>(r.status),
                   static_cast<int>(FlateStatus::BadData));
}

PDTK_TEST(gzip_container_is_refused) {
    // PDF /FlateDecode is the zlib wrapper (RFC 1950); a gzip container
    // (RFC 1952) must not be silently accepted.
    const std::vector<std::uint8_t> g = gzip_wrap(make_mixed(50'000, 23));
    FlateDecompressor dec;
    BumpArena arena(1u << 20);
    const FlateResult r = run(dec, g, arena, FlateLimits{});
    PDTK_ASSERT_EQ(static_cast<int>(r.status),
                   static_cast<int>(FlateStatus::BadData));
}

PDTK_TEST(empty_input_is_bad_data) {
    FlateDecompressor dec;
    BumpArena arena(4096);
    const FlateResult r =
        run(dec, std::vector<std::uint8_t>{}, arena, FlateLimits{});
    PDTK_ASSERT_EQ(static_cast<int>(r.status),
                   static_cast<int>(FlateStatus::BadData));
}

// --- bomb defenses (audit step 4) ----------------------------------------------

// Every refusal must leave the arena exactly at its mark — a refused
// stream may not leak staging bytes.
void assert_refusal_leaves_no_trace(FlateStatus want,
                                    const std::vector<std::uint8_t>& z,
                                    const FlateLimits& limits) {
    FlateDecompressor dec;
    BumpArena arena(16u << 20);
    const std::size_t mark = arena.used_bytes();
    const FlateResult r = run(dec, z, arena, limits);
    PDTK_ASSERT_EQ(static_cast<int>(r.status), static_cast<int>(want));
    PDTK_ASSERT_EQ(arena.used_bytes(), mark);
}

PDTK_TEST(ratio_limit_aborts_bomb) {
    // Zeros compress ~1000x: a 2 MiB payload in ~2 KiB. The default
    // 128x ratio cap must refuse it.
    std::vector<std::uint8_t> plain(2u << 20, 0x00);
    const std::vector<std::uint8_t> z = zlib_wrap(plain);
    PDTK_ASSERT(z.size() < plain.size() / 128);
    assert_refusal_leaves_no_trace(FlateStatus::LimitExceeded, z,
                                   FlateLimits{});
}

PDTK_TEST(absolute_cap_aborts_bomb) {
    const std::vector<std::uint8_t> plain = make_mixed(1u << 20, 31);
    const std::vector<std::uint8_t> z = zlib_wrap(plain);  // ~3-4x ratio
    FlateLimits limits;  // ratio is satisfied, the absolute cap is not
    limits.max_output_bytes = 64 << 10;
    PDTK_ASSERT(plain.size() > limits.max_output_bytes);
    assert_refusal_leaves_no_trace(FlateStatus::LimitExceeded, z, limits);
}

PDTK_TEST(size_hint_is_a_hard_bound) {
    const std::vector<std::uint8_t> plain = make_mixed(256u << 10, 32);
    const std::vector<std::uint8_t> z = zlib_wrap(plain);
    FlateLimits limits;
    limits.expected_size_hint = 1024;  // metadata lies (too small)
    assert_refusal_leaves_no_trace(FlateStatus::LimitExceeded, z, limits);
}

PDTK_TEST(exact_hint_is_single_shot) {
    const std::vector<std::uint8_t> plain = make_mixed(100'000, 33);
    const std::vector<std::uint8_t> z = zlib_wrap(plain);
    FlateLimits limits;
    limits.expected_size_hint = plain.size();  // metadata is exact
    FlateDecompressor dec;
    BumpArena arena(1u << 20);
    const std::size_t mark = arena.used_bytes();
    const FlateResult r = run(dec, z, arena, limits);
    PDTK_ASSERT_EQ(static_cast<int>(r.status), static_cast<int>(FlateStatus::Ok));
    PDTK_ASSERT_EQ(r.output.size(), plain.size());
    // One chunk, no growth, byte-aligned: consumption is exact.
    PDTK_ASSERT_EQ(arena.used_bytes() - mark, plain.size());
}

PDTK_TEST(generous_hint_still_succeeds) {
    const std::vector<std::uint8_t> plain = make_mixed(64u << 10, 34);
    const std::vector<std::uint8_t> z = zlib_wrap(plain);
    FlateLimits limits;
    limits.expected_size_hint = 8u << 20;  // metadata overshoots
    FlateDecompressor dec;
    BumpArena arena(16u << 20);
    const FlateResult r = run(dec, z, arena, limits);
    PDTK_ASSERT_EQ(static_cast<int>(r.status), static_cast<int>(FlateStatus::Ok));
    PDTK_ASSERT_EQ(r.output.size(), plain.size());
}

PDTK_TEST(zero_budget_refuses_immediately) {
    const std::vector<std::uint8_t> z = zlib_wrap(make_mixed(1000, 35));
    FlateLimits limits;
    limits.max_output_bytes = 0;
    assert_refusal_leaves_no_trace(FlateStatus::LimitExceeded, z, limits);
}

PDTK_TEST(arena_exhaustion_is_a_limit_refusal) {
    const std::vector<std::uint8_t> plain = make_mixed(64u << 10, 36);
    const std::vector<std::uint8_t> z = zlib_wrap(plain);
    FlateDecompressor dec;
    BumpArena arena(4096);  // far below the output size
    const FlateResult r = run(dec, z, arena, FlateLimits{});
    PDTK_ASSERT_EQ(static_cast<int>(r.status),
                   static_cast<int>(FlateStatus::LimitExceeded));
    PDTK_ASSERT_EQ(arena.used_bytes(), std::size_t{0});
}

PDTK_TEST(moved_instance_still_decompresses) {
    const std::vector<std::uint8_t> plain = make_mixed(50'000, 37);
    const std::vector<std::uint8_t> z = zlib_wrap(plain);
    FlateDecompressor first;
    FlateDecompressor second(std::move(first));
    BumpArena arena(1u << 20);
    const FlateResult r = run(second, z, arena, FlateLimits{});
    PDTK_ASSERT_EQ(static_cast<int>(r.status), static_cast<int>(FlateStatus::Ok));
    PDTK_ASSERT_EQ(r.output.size(), plain.size());

    // Move-assignment path.
    FlateDecompressor third;
    third = std::move(second);
    arena.reset();
    const FlateResult r2 = run(third, z, arena, FlateLimits{});
    PDTK_ASSERT_EQ(static_cast<int>(r2.status), static_cast<int>(FlateStatus::Ok));
}

PDTK_TEST(hostile_buffers_decompress_deterministically) {
    FlateDecompressor dec;
    BumpArena arena(4u << 20);
    for (std::uint32_t round = 0; round < 200; ++round) {
        Rng rng(0xF1A7E + round);
        const std::size_t size = 1 + rng.below(20'000);
        std::vector<std::uint8_t> plain(size);
        for (std::size_t i = 0; i < size; ++i) {
            plain[i] = static_cast<std::uint8_t>(rng.next());
        }
        const std::vector<std::uint8_t> z = zlib_wrap(plain);
        arena.reset();
        const FlateResult a = run(dec, z, arena, FlateLimits{});
        PDTK_ASSERT_EQ(static_cast<int>(a.status),
                       static_cast<int>(FlateStatus::Ok));
        const std::vector<std::uint8_t> snapshot(a.output.begin(),
                                                 a.output.end());
        arena.reset();
        const FlateResult b = run(dec, z, arena, FlateLimits{});
        PDTK_ASSERT_EQ(static_cast<int>(b.status),
                       static_cast<int>(FlateStatus::Ok));
        PDTK_ASSERT_EQ(b.output.size(), snapshot.size());
        PDTK_ASSERT(std::memcmp(b.output.data(), snapshot.data(),
                                snapshot.size()) == 0);
        PDTK_ASSERT_EQ(b.output.size(), plain.size());
        PDTK_ASSERT(std::memcmp(b.output.data(), plain.data(),
                                plain.size()) == 0);
    }
}

#else  // !PDTK_HAVE_FLATE — offline stub semantics

PDTK_TEST(stub_reports_unavailable) {
    PDTK_ASSERT(!FlateDecompressor::supported());
    FlateDecompressor dec;
    BumpArena arena(4096);
    const FlateResult r =
        dec.decompress(std::span<const std::uint8_t>(
                           std::vector<std::uint8_t>{0x78, 0x01}),
                       arena, FlateLimits{});
    PDTK_ASSERT_EQ(static_cast<int>(r.status),
                   static_cast<int>(FlateStatus::Unavailable));
    PDTK_ASSERT(std::strlen(r.message) > 0);
}

#endif  // PDTK_HAVE_FLATE

}  // namespace

PDTK_TEST_MAIN()
