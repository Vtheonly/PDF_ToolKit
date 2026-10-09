// Hardware-accelerated Flate decompressor (audit issue-1/task-2.4).
//
// libdeflate (the canonical C library behind the audit's "libdeflater"
// name — see ADR-0007 and the problem registry) provides SIMD-dispatched
// one-shot inflate. Two shapes of this TU exist:
//
//   * PDTK_HAVE_FLATE=1 (PDTK_ENABLE_FLATE=ON, default): the real
//     decompressor. Output is staged in doubling chunks inside the
//     caller's BumpArena (audit step 3), growing via BumpArena::
//     rewind_to so a retry reuses the failed chunk's bytes; the
//     decompression-bomb defenses (audit step 4) cap the growth.
//
//   * PDTK_HAVE_FLATE=0 (offline profile, ADR-0003/0007): an honest
//     stub — every decompress() returns Unavailable and callers keep
//     their degradation paths (the xref resolver's emergency linear
//     scan). Nothing fakes success.

#include "pdftoolkit/codec/flate.hpp"

#ifndef PDTK_HAVE_FLATE
#define PDTK_HAVE_FLATE 0
#endif

#if PDTK_HAVE_FLATE

#include <limits>

#include "libdeflate.h"

namespace pdftoolkit::codec {
namespace {

// First speculative chunk when no size hint exists. 64 KiB covers the
// row payload of every realistic xref stream in one shot.
constexpr std::size_t kInitialChunkBytes = std::size_t{64} << 10;

// Output-size guess as a multiple of the compressed size when no hint
// exists. Measured on this engine's workloads (benchmark recording
// 2026-10-09-phase2-flate-baseline): PDF text content compresses ~3-6x,
// so a 4x guess guarantees a wasted aborted attempt on EVERY such
// stream (fill 4x, rewind, redo) — 45% of the inflate work thrown away.
// 8x single-shots the common content ratios; higher ratios amortize
// >= 8/(8+ratio) of the retry work (93% at 100x).
constexpr std::uint64_t kGuessMultiplier = 8;

// Saturating multiply-add used to build the output cap.
std::size_t sat_mul(std::uint64_t a, std::uint64_t b) noexcept {
    const std::uint64_t product = a * b;  // no overflow: both <= 2^40
    if (product >= std::numeric_limits<std::size_t>::max()) {
        return std::numeric_limits<std::size_t>::max();
    }
    return static_cast<std::size_t>(product);
}

std::size_t sat_min(std::size_t a, std::uint64_t b) noexcept {
    if (b >= std::numeric_limits<std::size_t>::max()) {
        return a;
    }
    const auto wide = static_cast<std::size_t>(b);
    return a < wide ? a : wide;
}

}  // namespace

bool FlateDecompressor::supported() noexcept { return true; }

FlateDecompressor::FlateDecompressor() noexcept
    : decompressor_(libdeflate_alloc_decompressor()) {}

FlateDecompressor::~FlateDecompressor() {
    libdeflate_free_decompressor(decompressor_);
}

FlateDecompressor::FlateDecompressor(FlateDecompressor&& other) noexcept
    : decompressor_(other.decompressor_) {
    other.decompressor_ = nullptr;
}

FlateDecompressor& FlateDecompressor::operator=(
    FlateDecompressor&& other) noexcept {
    if (this != &other) {
        libdeflate_free_decompressor(decompressor_);
        decompressor_ = other.decompressor_;
        other.decompressor_ = nullptr;
    }
    return *this;
}

FlateResult FlateDecompressor::decompress(
    std::span<const std::uint8_t> input, memory::BumpArena& arena,
    const FlateLimits& limits) noexcept {
    if (decompressor_ == nullptr) {
        return {FlateStatus::Unavailable, {}, "decompressor allocation failed"};
    }
    if (input.empty()) {
        return {FlateStatus::BadData, {}, "empty Flate stream"};
    }

    // ---- bomb defenses (audit step 4): the output cap -----------------
    //
    // cap = min(absolute budget, ratio x compressed, hint). Every chunk
    // attempt is <= cap, so more output than the cap can never be
    // produced or touched. A zero cap means the defenses refuse the
    // stream outright.
    std::size_t cap = limits.max_output_bytes;
    if (limits.expansion_ratio > 0) {
        cap = sat_min(cap, sat_mul(input.size(), limits.expansion_ratio));
    }
    if (limits.expected_size_hint > 0) {
        cap = sat_min(cap, limits.expected_size_hint);
    }
    if (cap == 0) {
        return {FlateStatus::LimitExceeded, {},
                "Flate output budget is zero (bomb defenses)"};
    }

    // ---- sizing --------------------------------------------------------
    //
    // With a trusted hint the first chunk matches it exactly (one
    // shot); otherwise start at 8x the compressed size (floor 64 KiB).
    std::size_t guess = sat_min(
        std::max(kInitialChunkBytes, sat_mul(input.size(), kGuessMultiplier)),
        cap);
    if (limits.expected_size_hint > 0) {
        guess = sat_min(
            std::max<std::size_t>(1, static_cast<std::size_t>(
                                         limits.expected_size_hint)),
            cap);
    }
    if (guess == 0) {
        guess = 1;
    }

    const std::size_t mark = arena.used_bytes();
    // Output is strictly greater than `known_min` once an attempt has
    // filled a chunk (drives both the doubling growth and the
    // fit-to-arena adaptation below).
    std::size_t known_min = 0;
    while (true) {
        arena.rewind_to(mark);
        const std::span<std::uint8_t> chunk =
            arena.alloc_slice<std::uint8_t>(guess);
        if (chunk.empty()) {
            // The arena cannot serve `guess` — but the stream may still
            // fit what remains (e.g. an incompressible stream whose 8x
            // guess wildly overshoots its ~1x output). Retry with
            // exactly the remaining capacity; refuse only when even
            // that is known-insufficient.
            const std::size_t remaining =
                arena.capacity_bytes() > mark ? arena.capacity_bytes() - mark
                                              : 0;
            if (remaining > known_min && remaining < guess) {
                guess = remaining;
                continue;
            }
            return {FlateStatus::LimitExceeded, {},
                    "arena exhausted below the Flate output cap"};
        }

        std::size_t written = 0;
        const int rc = libdeflate_zlib_decompress(
            decompressor_, input.data(), input.size(), chunk.data(),
            chunk.size(), &written);

        switch (rc) {
            case LIBDEFLATE_SUCCESS:
                return {FlateStatus::Ok, chunk.first(written),
                        "ok"};
            case LIBDEFLATE_BAD_DATA:
                // Contract: a refused stream leaves the arena exactly
                // at its mark — no dead staging bytes survive.
                arena.rewind_to(mark);
                return {FlateStatus::BadData, {},
                        "corrupt or truncated Flate stream"};
            case LIBDEFLATE_INSUFFICIENT_SPACE:
                known_min = guess;
                if (guess >= cap) {
                    arena.rewind_to(mark);
                    return {FlateStatus::LimitExceeded, {},
                            "Flate output exceeds the bomb-defense cap"};
                }
                guess = sat_min(std::max<std::size_t>(guess * 2, guess + 1),
                                cap);
                continue;
            default:
                // SHORT_OUTPUT cannot occur (actual_out_nbytes_ret is
                // non-null); any other value is a libdeflate contract
                // violation — refuse rather than guess.
                arena.rewind_to(mark);
                return {FlateStatus::BadData, {},
                        "unexpected libdeflate result code"};
        }
    }
}

}  // namespace pdftoolkit::codec

#else  // !PDTK_HAVE_FLATE — honest offline stub (ADR-0007)

namespace pdftoolkit::codec {

bool FlateDecompressor::supported() noexcept { return false; }

FlateDecompressor::FlateDecompressor() noexcept : decompressor_(nullptr) {}
FlateDecompressor::~FlateDecompressor() = default;
FlateDecompressor::FlateDecompressor(FlateDecompressor&& other) noexcept
    : decompressor_(other.decompressor_) {
    other.decompressor_ = nullptr;
}
FlateDecompressor& FlateDecompressor::operator=(
    FlateDecompressor&& other) noexcept {
    if (this != &other) {
        decompressor_ = other.decompressor_;
        other.decompressor_ = nullptr;
    }
    return *this;
}

FlateResult FlateDecompressor::decompress(
    std::span<const std::uint8_t> /*input*/,
    memory::BumpArena& /*arena*/,
    const FlateLimits& /*limits*/) noexcept {
    return {FlateStatus::Unavailable, {},
            "built without libdeflate (PDTK_ENABLE_FLATE=OFF)"};
}

}  // namespace pdftoolkit::codec

#endif  // PDTK_HAVE_FLATE
