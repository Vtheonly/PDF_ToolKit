#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "pdftoolkit/memory/arena.hpp"

/// Opaque libdeflate decompressor state (defined in the TU; the public
/// header never includes libdeflate.h — the codec is an engine-internal
/// detail, not a leaked third-party API).
struct libdeflate_decompressor;

namespace pdftoolkit::codec {

/// Outcome of a Flate decompression attempt.
enum class FlateStatus {
    /// Success — `FlateResult::output` holds the decompressed bytes.
    Ok,
    /// The stream is not a valid zlib/Flate stream (corrupt, truncated
    /// or unsupported). libdeflate cannot distinguish truncation from
    /// corruption; both surface here.
    BadData,
    /// A decompression-bomb defense refused the stream before it could
    /// exceed its output budget: the expansion-ratio cap, the absolute
    /// output cap or the caller-supplied size hint (audit task 2.4
    /// step 4), or the arena ran out of capacity below the cap.
    LimitExceeded,
    /// Built with `PDTK_ENABLE_FLATE=OFF` (offline profile): no
    /// decompressor exists. Callers degrade honestly (e.g. the xref
    /// resolver falls back to the emergency linear scan).
    Unavailable,
};

/// Decompression-bomb defenses (audit task 2.4 step 4) and sizing hints.
///
/// The audit's two hard aborts — "decompressed bytes exceed 128x
/// compressed size" and "the engine's per-page memory budget (e.g.
/// 64 MB)" — are the defaults; the metadata check ("uncompressed length
/// /Length1 if present") is the caller-supplied `expected_size_hint`
/// (0 = no metadata: absent for xref streams, /Length1 for embedded
/// font programs in later phases). A present hint is a HARD bound, per
/// the audit: decompressing past it aborts with LimitExceeded. Callers
/// must only pass metadata they are prepared to trust.
struct FlateLimits {
    /// Maximum decompressed/compressed ratio (audit: 128).
    std::uint64_t expansion_ratio = 128;
    /// Absolute output budget in bytes (audit's per-page budget: 64 MiB).
    std::size_t max_output_bytes = std::size_t{64} << 20;
    /// Trusted uncompressed-size hint from stream metadata, 0 = absent.
    std::uint64_t expected_size_hint = 0;
};

/// Result of `FlateDecompressor::decompress`. `message` is a stable
/// diagnostic (never null); `output` is valid only for FlateStatus::Ok
/// and points INTO the caller's arena (zero-copy: valid until the next
/// rewind/reset of allocations past its mark).
struct FlateResult {
    FlateStatus status = FlateStatus::Unavailable;
    std::span<std::uint8_t> output{};
    const char* message = "";
};

/// Hardware-accelerated Flate (zlib-wrapped DEFLATE, RFC 1950 — the PDF
/// /FlateDecode stream format) decompressor (audit task 2.4).
///
/// Backed by libdeflate (SIMD-dispatched inflate; ADR-0007). The output
/// is staged in chunks inside the caller's BumpArena: the decompressed
/// size is unknown up front, so the decompressor speculates a chunk
/// (8x the compressed size, or the caller's trusted hint), retries with
/// a doubled chunk on LIBDEFLATE_INSUFFICIENT_SPACE and rewinds the
/// arena between attempts — arena consumption stays at the final chunk
/// size, not the sum of the attempts. When a speculative chunk exceeds
/// the arena's remaining capacity, the codec retries with exactly that
/// capacity before refusing (an incompressible stream's 8x guess can
/// overshoot a small arena while its ~1x output fits). The bomb
/// defenses cap the chunk growth, so a bomb dies at its cap without
/// ever touching more than `min(cap, arena capacity)` bytes.
///
/// One instance per thread is the intended ownership (the arena is
/// thread-local by design); instances are cheap to construct but not
/// free, so reuse one across streams of a query where natural.
class FlateDecompressor {
public:
    /// Whether this build carries a real decompressor
    /// (`PDTK_ENABLE_FLATE`, ADR-0007). Static so callers can pick
    /// their degradation path without constructing an instance.
    [[nodiscard]] static bool supported() noexcept;

    FlateDecompressor() noexcept;
    ~FlateDecompressor();

    FlateDecompressor(const FlateDecompressor&) = delete;
    FlateDecompressor& operator=(const FlateDecompressor&) = delete;

    FlateDecompressor(FlateDecompressor&& other) noexcept;
    FlateDecompressor& operator=(FlateDecompressor&& other) noexcept;

    /// Decompresses the zlib-wrapped Flate stream `input` into `arena`
    /// under `limits`. Never throws; never touches memory outside the
    /// arena's remaining capacity. The returned span aliases the arena.
    /// A refused stream (any non-Ok status) leaves the arena exactly at
    /// its mark — no staging bytes survive a refusal.
    [[nodiscard]] FlateResult decompress(std::span<const std::uint8_t> input,
                                         memory::BumpArena& arena,
                                         const FlateLimits& limits) noexcept;

private:
    libdeflate_decompressor* decompressor_;
};

}  // namespace pdftoolkit::codec
