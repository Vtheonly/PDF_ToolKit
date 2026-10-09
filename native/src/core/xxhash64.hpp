// xxhash64.hpp — in-repo xxHash64 (xxHash specification, Yann Collet /
// Cyan4973, public domain spec) for engine-internal content hashing.
//
// INTERNAL header (lives under src/, never installed, never included
// from public headers) — the same contract as scan_util.hpp. First
// consumer: task 3.1 (CMap cache content dedup). Future consumers:
// task 3.3 (term hashing into the slab's term-hash section) and task
// 4.1 (corpus-level lexicon). Implemented in-repo rather than fetched:
// the algorithm is ~150 lines, spec-stable since 2016, and a third
// FetchContent dependency would buy nothing the offline guarantee
// cares about (policy recorded in ADR-0008).
//
// Known-answer tests: native/tests/test_cmap.cpp carries reference
// vectors generated from the canonical C implementation (Python
// `xxhash` wheel, xxh64, seed 0) — including lengths that cross the
// 32-byte stripe boundary (31/32/33/63/64/65) where the tail rules
// change. If this header is ever edited, those vectors are the proof
// nothing drifted.
//
// Arithmetic is plain wrapping uint64_t (unsigned overflow is the
// specified behaviour); byte reads are explicitly little-endian by
// assembly (portable; compilers fold them into single loads on LE
// targets). std::rotr/std::rotl are the C++20 <bit> primitives.

#ifndef PDFTOOLKIT_HASH_XXHASH64_HPP
#define PDFTOOLKIT_HASH_XXHASH64_HPP

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pdftoolkit::hash {

inline namespace xxhash64_detail {

inline constexpr std::uint64_t kPrime64_1 = 0x9E3779B185EBCA87ULL;
inline constexpr std::uint64_t kPrime64_2 = 0xC2B2AE3D27D4EB4FULL;
inline constexpr std::uint64_t kPrime64_3 = 0x165667B19E3779F9ULL;
inline constexpr std::uint64_t kPrime64_4 = 0x85EBCA77C2B2AE63ULL;
inline constexpr std::uint64_t kPrime64_5 = 0x27D4EB2F165667C5ULL;

// Explicit little-endian reads (the xxHash spec fixes LE on all
// platforms). The byte-assembly form is endian-portable and MSan-clean.
inline std::uint64_t read_le64(const std::uint8_t* p) noexcept {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | static_cast<std::uint64_t>(p[i]);
    }
    return v;
}

inline std::uint64_t read_le32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint64_t>(p[0]) |
           (static_cast<std::uint64_t>(p[1]) << 8) |
           (static_cast<std::uint64_t>(p[2]) << 16) |
           (static_cast<std::uint64_t>(p[3]) << 24);
}

// One accumulator round over one little-endian lane value (the XXH64
// stripe path feeds 8-byte lanes; the tail feeds 8- or 4-byte values;
// the byte path multiplies single bytes).
inline std::uint64_t round64(std::uint64_t acc,
                             std::uint64_t lane) noexcept {
    acc += lane * kPrime64_2;
    acc = std::rotl(acc, 31);
    return acc * kPrime64_1;
}

// Merge one accumulator into the running hash (spec step 4.1.2).
inline std::uint64_t merge_round64(std::uint64_t acc,
                                   std::uint64_t val) noexcept {
    val = round64(0, val);
    acc ^= val;
    return acc * kPrime64_1 + kPrime64_4;
}

inline std::uint64_t avalanche64(std::uint64_t h) noexcept {
    h ^= h >> 33;
    h *= kPrime64_2;
    h ^= h >> 29;
    h *= kPrime64_3;
    h ^= h >> 32;
    return h;
}

}  // namespace xxhash64_detail

/// Streaming xxHash64 builder. `update` any number of byte ranges;
/// `digest` is idempotent (const — the state is never consumed).
///
///   XxHash64 h;
///   h.update(a); h.update(b);
///   std::uint64_t key = h.digest();   // == xxhash64(a || b)
///
/// Use a non-zero seed as a DOMAIN SEPARATOR when the same bytes must
/// never produce the same key across different roles (e.g. task 3.1's
/// CMap dedup keys tag raw vs Flate payloads with different seeds).
class XxHash64 {
public:
    explicit XxHash64(std::uint64_t seed = 0) noexcept
        : v1_(seed + kPrime64_1 + kPrime64_2),
          v2_(seed + kPrime64_2),
          v3_(seed),
          v4_(seed - kPrime64_1),
          seed_(seed) {}

    void update(std::span<const std::uint8_t> data) noexcept {
        total_ += data.size();
        const std::uint8_t* p = data.data();
        std::size_t n = data.size();

        // Top up a partial buffer first (0 < fill_ < 32 only between
        // update calls; a fresh fill_ == 32 is consumed below).
        if (fill_ > 0) {
            const std::size_t take =
                n < (kStripe - fill_) ? n : (kStripe - fill_);
            for (std::size_t i = 0; i < take; ++i) {
                buffer_[fill_ + i] = p[i];
            }
            fill_ += take;
            p += take;
            n -= take;
            if (fill_ < kStripe) {
                return;  // still partial — wait for more input
            }
            consume_stripe(buffer_);
            fill_ = 0;
        }

        // Whole stripes straight from the input (no staging copy).
        while (n >= kStripe) {
            consume_stripe(p);
            p += kStripe;
            n -= kStripe;
        }

        // Park the tail (< 32 bytes) for digest().
        for (std::size_t i = 0; i < n; ++i) {
            buffer_[i] = p[i];
        }
        fill_ = n;
    }

    [[nodiscard]] std::uint64_t digest() const noexcept {
        std::uint64_t h;
        if (total_ >= kStripe) {
            h = std::rotl(v1_, 1) + std::rotl(v2_, 7) +
                std::rotl(v3_, 12) + std::rotl(v4_, 18);
            h = merge_round64(h, v1_);
            h = merge_round64(h, v2_);
            h = merge_round64(h, v3_);
            h = merge_round64(h, v4_);
        } else {
            h = seed_ + kPrime64_5;
        }
        h += static_cast<std::uint64_t>(total_);

        // Tail rules in descending alignment (spec step 4.2).
        std::size_t pos = 0;
        std::size_t left = fill_;
        while (left >= 8) {
            h ^= round64(0, read_le64(buffer_ + pos));
            h = std::rotl(h, 27) * kPrime64_1 + kPrime64_4;
            pos += 8;
            left -= 8;
        }
        if (left >= 4) {
            h ^= read_le32(buffer_ + pos) * kPrime64_1;
            h = std::rotl(h, 23) * kPrime64_2 + kPrime64_3;
            pos += 4;
            left -= 4;
        }
        while (left > 0) {
            h ^= static_cast<std::uint64_t>(buffer_[pos]) * kPrime64_5;
            h = std::rotl(h, 11) * kPrime64_1;
            ++pos;
            --left;
        }
        return avalanche64(h);
    }

private:
    static constexpr std::size_t kStripe = 32;

    // One 32-byte stripe = four 8-byte little-endian lanes, one round
    // each: acc1<-[0..8), acc2<-[8..16), acc3<-[16..24), acc4<-[24..32).
    // (NOT eight 4-byte lanes round-robin — that is XXH32's shape; the
    // reference vectors caught this misreading before it shipped.)
    void consume_stripe(const std::uint8_t* p) noexcept {
        v1_ = round64(v1_, read_le64(p));
        v2_ = round64(v2_, read_le64(p + 8));
        v3_ = round64(v3_, read_le64(p + 16));
        v4_ = round64(v4_, read_le64(p + 24));
    }

    std::uint64_t v1_;
    std::uint64_t v2_;
    std::uint64_t v3_;
    std::uint64_t v4_;
    std::uint64_t seed_;
    std::uint64_t total_ = 0;
    std::uint8_t buffer_[kStripe];
    std::size_t fill_ = 0;
};

/// One-shot convenience over a byte range.
[[nodiscard]] inline std::uint64_t xxhash64(
    std::span<const std::uint8_t> data,
    std::uint64_t seed = 0) noexcept {
    XxHash64 h(seed);
    h.update(data);
    return h.digest();
}

}  // namespace pdftoolkit::hash

#endif  // PDFTOOLKIT_HASH_XXHASH64_HPP
