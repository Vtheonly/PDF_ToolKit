#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include <span>

namespace pdftoolkit::memory {

/// Thread-local monolithic bump-pointer arena (audit issue-1/task-1.2).
///
/// Purpose: eliminate system allocator calls from query execution paths.
/// All slices handed out live inside one 64-byte-aligned allocation; a
/// single `reset()` deallocates everything in O(1). Not thread-safe by
/// design — one arena per worker thread (thread-local storage).
///
/// Differences from the audit's reference sketch (documented deviations,
/// all hardening):
///   * capacity is rounded up to a multiple of the 64-byte alignment —
///     `std::aligned_alloc` requires the size to be a multiple of the
///     alignment (C11 requirement the sketch violated);
///   * the class is non-copyable and movable — the sketch allowed
///     accidental double-free via copies;
///   * `count * sizeof(T)` overflow is checked before the span arithmetic.
class BumpArena {
public:
    static constexpr std::size_t kAlignment = 64;

    explicit BumpArena(std::size_t capacity)
        : buffer_(allocate(round_up(capacity))),
          capacity_(round_up(capacity)),
          offset_(0) {
        if (buffer_ == nullptr) {
            capacity_ = 0;
        }
    }

    ~BumpArena() { release(buffer_); }

    BumpArena(const BumpArena&) = delete;
    BumpArena& operator=(const BumpArena&) = delete;

    BumpArena(BumpArena&& other) noexcept
        : buffer_(std::exchange(other.buffer_, nullptr)),
          capacity_(std::exchange(other.capacity_, std::size_t{0})),
          offset_(std::exchange(other.offset_, std::size_t{0})) {}

    BumpArena& operator=(BumpArena&& other) noexcept {
        if (this != &other) {
            release(buffer_);
            buffer_ = std::exchange(other.buffer_, nullptr);
            capacity_ = std::exchange(other.capacity_, std::size_t{0});
            offset_ = std::exchange(other.offset_, std::size_t{0});
        }
        return *this;
    }

    /// Allocate a contiguous slice for `count` elements of T, honouring
    /// alignof(T) (<= kAlignment, enforced at compile time). Returns an
    /// empty span when the arena is exhausted. Never throws.
    template <typename T>
    std::span<T> alloc_slice(std::size_t count) noexcept {
        static_assert(alignof(T) <= kAlignment,
                      "BumpArena serves alignments up to 64 bytes only");

        if (count > static_cast<std::size_t>(-1) / sizeof(T)) {
            return {};  // count * sizeof(T) would overflow
        }
        const std::size_t bytes = count * sizeof(T);
        const std::size_t current = offset_;
        const std::size_t aligned =
            (current + (alignof(T) - 1)) & ~static_cast<std::size_t>(alignof(T) - 1);
        // Invariant: offset_ <= capacity_ and capacity_ is a multiple of 64
        // (hence of every power-of-two alignment <= 64), so aligned <= capacity_.
        if (aligned > capacity_ || bytes > capacity_ - aligned) {
            return {};
        }
        offset_ = aligned + bytes;
        return std::span<T>(reinterpret_cast<T*>(buffer_ + aligned), count);
    }

    /// O(1) bulk deallocation of every slice handed out so far.
    void reset() noexcept { offset_ = 0; }

    /// Rewinds the bump pointer to a mark captured earlier via
    /// `used_bytes()` (task 2.4: speculative sub-allocations that may
    /// need to grow, e.g. decompression staging chunks). Only ever
    /// rewinds: a mark at or past the current offset is ignored, so a
    /// stale mark can never push the pointer forward or past the
    /// capacity invariant. Slices handed out after `mark` become
    /// logically dead — the caller guarantees nobody still holds them.
    /// The next `alloc_slice` re-aligns, so a mid-alignment mark is
    /// legal (it only costs a few bytes of padding).
    void rewind_to(std::size_t mark) noexcept {
        if (mark < offset_) {
            offset_ = mark;
        }
    }

    [[nodiscard]] std::size_t used_bytes() const noexcept { return offset_; }
    [[nodiscard]] std::size_t capacity_bytes() const noexcept { return capacity_; }
    [[nodiscard]] bool valid() const noexcept { return buffer_ != nullptr; }

    /// Whether `p` lies inside this arena's allocation (diagnostics/tests).
    [[nodiscard]] bool owns(const void* p) const noexcept {
        const auto* byte = static_cast<const unsigned char*>(p);
        return byte >= buffer_ && byte < buffer_ + capacity_;
    }

private:
    static std::size_t round_up(std::size_t n) noexcept {
        return (n + kAlignment - 1) & ~static_cast<std::size_t>(kAlignment - 1);
    }

    static unsigned char* allocate(std::size_t bytes) noexcept;
    static void release(unsigned char* buffer) noexcept;

    unsigned char* buffer_;
    std::size_t capacity_;
    std::size_t offset_;
};

}  // namespace pdftoolkit::memory
