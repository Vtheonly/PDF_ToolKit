#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#ifndef _WIN32
#include <setjmp.h>  // sigjmp_buf for the guarded() recovery point
#endif

#include "pdftoolkit/errors.hpp"

namespace pdftoolkit::memory {

/// Kernel read-ahead hints (audit task 1.1, step 3):
/// WILLNEED during sequential corpus ingestion, RANDOM for index-directed
/// ad-hoc reads.
enum class Advice {
    WillNeed,  ///< posix_madvise(MADV_WILLNEED): prefetch pages
    Random,    ///< posix_madvise(MADV_RANDOM): disable read-ahead
};

namespace detail {

/// RAII publisher of the thread-local SIGBUS recovery point used by
/// MmapHandle::guarded(). Out-of-line in mmap.cpp so the public header
/// carries no signal-handler machinery.
struct GuardInstall {
    GuardInstall(void* buf) noexcept;
    ~GuardInstall();
    GuardInstall(const GuardInstall&) = delete;
    GuardInstall& operator=(const GuardInstall&) = delete;
};

}  // namespace detail

/// Guarded memory-mapped file (audit task 1.1).
///
/// Contract:
///   * opens `path` read-only and maps it privately (zero-copy, no
///     kernel-to-user copies on read);
///   * memory is exposed exclusively as an immutable
///     std::span<const uint8_t>;
///   * a zero-length file yields a VALID handle with an empty span (no
///     mapping and no guard-table slot are created for it);
///   * kernel hints via advise() are advisory and never throw;
///   * SIGBUS truncation protection is OPT-IN per access region through
///     guarded(); a fault inside a registered mapping under an active
///     guard is converted to PdfToolkitException(IoTruncated).
///
/// Constructor failure mapping (PdfToolkitException):
///   null/empty path                -> InvalidArgument
///   ENOENT                         -> DocumentNotFound
///   EACCES and other "cannot read" -> UnreadablePdf
///   mmap failure                   -> Internal (message carries errno)
///   guard-table exhaustion         -> Internal (see mmap.cpp; 128 slots)
///
/// Lifetime note: POSIX keeps the descriptor open for the mapping's
/// lifetime and releases everything in ~MmapHandle; move transfers
/// ownership (the mapping address never moves).
class MmapHandle {
public:
    explicit MmapHandle(const char* path);
    ~MmapHandle();

    MmapHandle(const MmapHandle&) = delete;
    MmapHandle& operator=(const MmapHandle&) = delete;

    MmapHandle(MmapHandle&& other) noexcept;
    MmapHandle& operator=(MmapHandle&& other) noexcept;

    /// Immutable zero-copy view of the file bytes.
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
        return std::span<const std::uint8_t>(data_, length_);
    }
    [[nodiscard]] std::size_t size() const noexcept { return length_; }
    /// True while this handle owns a live file resource or mapping
    /// (includes the zero-length-file case).
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const char* path() const noexcept { return path_.c_str(); }
    [[nodiscard]] bool owns(const void* p) const noexcept;

    /// Kernel read-ahead hint; advisory only. Returns false when the
    /// advice could not be delivered (unmapped handle, kernel refusal, or
    /// Windows where no equivalent exists) — never throws.
    bool advise(Advice advice) noexcept;

    /// Executes `f` under SIGBUS truncation protection (POSIX only).
    ///
    /// If the file is truncated under the mapping while `f` runs, and the
    /// fault lands inside a registered mapping while this thread has an
    /// active recovery point, the fault is converted into
    /// PdfToolkitException(IoTruncated) thrown from here — the audit task
    /// 1.1 acceptance criterion.
    ///
    /// Honesty contract (locked by native/tests/test_mmap.cpp):
    /// faults in foreign memory, faults with no active recovery point,
    /// and faults after the handle died are re-raised with the default
    /// disposition — protection is opt-in, never a blanket swallow.
    ///
    /// Must not be nested: a nested call overwrites the recovery point
    /// (last installation wins) and longjmp would skip inner destructors
    /// (C++ undefined behaviour). Documented; callers keep guards at one
    /// level.
    template <typename F>
    decltype(auto) guarded(F&& f) {
#ifdef _WIN32
        // Windows: SEH translation of EXCEPTION_IN_PAGE_ERROR is deferred
        // (compile-only path, unknowns U-011); guarded() runs f unguarded.
        return std::forward<F>(f)();
#else
        sigjmp_buf recovery;
        // The order here is load-bearing (ADR-0006): the recovery point
        // must be established by sigsetjmp BEFORE GuardInstall publishes
        // it to the signal handler — otherwise a SIGBUS arriving in that
        // window would siglongjmp into an uninitialised buffer. And
        // GuardInstall must live INSIDE the setjmp scope so its
        // destructor runs on the normal path. The longjmp path skips
        // that destructor, which is safe by construction: the handler
        // nulls the thread-local pointer before jumping — the
        // destructor's only effect.
        if (sigsetjmp(recovery, 1) == 0) {
            detail::GuardInstall install(&recovery);
            return std::forward<F>(f)();
        }
        // siglongjmp landed here: the handler already consumed the
        // recovery point; surface the typed truncation error.
        throw PdfToolkitException(ErrorCode::IoTruncated,
                                  "file truncated under memory map");
#endif
    }

private:
    void destroy() noexcept;

    std::string path_;
    const std::uint8_t* data_ = nullptr;
    std::size_t length_ = 0;
#ifdef _WIN32
    void* file_ = nullptr;   // HANDLE, kept open for the mapping lifetime
    void* section_ = nullptr;
#else
    int fd_ = -1;
#endif
};

}  // namespace pdftoolkit::memory
