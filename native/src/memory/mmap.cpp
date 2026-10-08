// MmapHandle — guarded memory-mapped file access (audit issue-1/task-1.1).
//
// POSIX path: open + fstat + mmap(PROT_READ, MAP_PRIVATE) + optional
// posix_madvise. Win32 path: CreateFileA + CreateFileMappingA +
// MapViewOfFile (compile-only in this environment, unknowns U-011).
//
// SIGBUS design (ADR-0006): the audit prescribes "a thread-safe sigaction
// handler for SIGBUS ... to catch asynchronous file truncation and throw
// PdfToolkitException(ErrorCode::IoTruncated)". Throwing a C++ exception
// from inside a signal handler is undefined behaviour (the unwinder can
// run on the signal alternate stack mid-syscall), so the typed throw
// happens on the normal stack instead: guarded() publishes a thread-local
// recovery point (sigjmp_buf) and the handler siglongjmp()s back to it
// when — and only when — the fault address lies inside a registered live
// mapping while this thread has an active recovery point. Everything
// else (foreign faults, no recovery point, dead handle) is re-raised
// with the disposition found at install time, so the process dies
// exactly as it would have without us. That honesty contract is
// specified in mmap.hpp and locked by native/tests/test_mmap.cpp.

#include "pdftoolkit/memory/mmap.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>

#ifndef _WIN32

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pdftoolkit::memory {

namespace {

// ---------------------------------------------------------------------------
// SIGBUS guard machinery (ADR-0006)
// ---------------------------------------------------------------------------

// Documented capacity (mmap.hpp): guard-table exhaustion fails handle
// construction with Internal instead of silently mapping unprotected.
constexpr std::size_t kGuardSlots = 128;

// One registered live mapping. The SIGBUS handler reads this table while
// other threads may be constructing/destroying handles, so each slot is
// a per-slot seqlock: a writer flips `version` odd around its mutation;
// the handler skips any slot whose version is odd or changed during the
// read. Skipping is the fail-safe direction — the fault is then treated
// as foreign and re-raised — and can only affect a fault racing with
// that exact slot's (un)registration, never a steady-state access.
// alignas(64): each slot on its own cache line keeps the handler's scan
// free of false sharing with the writer side.
struct alignas(64) GuardSlot {
    std::atomic<std::uint64_t> version{0};  // even = stable, odd = mutating
    const std::uint8_t* begin{nullptr};
    const std::uint8_t* end{nullptr};
};

GuardSlot g_guard_slots[kGuardSlots];

// Serialises slot allocation/teardown between writer threads. NEVER taken
// by the signal handler (async-signal-safety).
std::mutex g_guard_mutex;

// Thread-local recovery point published by MmapHandle::guarded(). Null
// outside a guarded region. The handler consumes it (nulls it) before
// jumping, which is also ~GuardInstall's only effect — the longjmp path
// skips that destructor, so this makes the skip harmless by construction.
thread_local sigjmp_buf* t_recovery = nullptr;

struct sigaction g_previous_sigbus;  // disposition found at install time
std::once_flag g_install_once;

// Integer comparison — pedantically defined for unrelated pointers
// (relational comparison of void* is not).
bool range_contains(const std::uint8_t* begin, const std::uint8_t* end,
                    const void* fault) noexcept {
    const auto f = reinterpret_cast<std::uintptr_t>(fault);
    return f >= reinterpret_cast<std::uintptr_t>(begin) &&
           f < reinterpret_cast<std::uintptr_t>(end);
}

// Async-signal-safe registry probe. Skips slots caught mid-mutation.
bool fault_in_registered_mapping(const void* fault) noexcept {
    for (const GuardSlot& slot : g_guard_slots) {
        const std::uint64_t before =
            slot.version.load(std::memory_order_acquire);
        if ((before & 1u) != 0u) {
            continue;  // writer active on this slot: fail-safe skip
        }
        const bool contains =
            range_contains(slot.begin, slot.end, fault) && slot.begin != nullptr;
        if (slot.version.load(std::memory_order_acquire) == before) {
            if (contains) {
                return true;
            }
        }  // else: raced with a writer — skip (fail-safe)
    }
    return false;
}

void sigbus_handler(int /*signo*/, siginfo_t* info, void* /*ucontext*/) noexcept {
    const void* fault = (info != nullptr) ? info->si_addr : nullptr;
    sigjmp_buf* recovery = t_recovery;

    if (fault == nullptr || recovery == nullptr ||
        !fault_in_registered_mapping(fault)) {
        // Honesty contract: not ours to swallow. Restore the disposition
        // found at install time (possibly a runtime's, e.g. a sanitizer's)
        // and re-raise so the process dies exactly as it would have
        // without us. SIGBUS is blocked while the handler runs — unblock
        // first, or the re-raise stays pending and never delivers.
        ::sigaction(SIGBUS, &g_previous_sigbus, nullptr);
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGBUS);
        ::sigprocmask(SIG_UNBLOCK, &mask, nullptr);
        ::raise(SIGBUS);
        // Unreachable for SIG_DFL and sanitizer dispositions; backstop in
        // case the previous disposition ignored the signal.
        ::_exit(128 + SIGBUS);
    }

    // Consume the recovery point (mirrors ~GuardInstall — see above),
    // then return to the normal stack where guarded() throws the typed
    // truncation error.
    t_recovery = nullptr;
    ::siglongjmp(*recovery, 1);
}

// Installs the handler once per process. Throws Internal when sigaction
// fails — a mapping must never exist without its guard handler active.
void install_sigbus_handler_once() {
    std::call_once(g_install_once, [] {
        struct sigaction act;
        std::memset(&act, 0, sizeof(act));
        act.sa_sigaction = sigbus_handler;
        sigemptyset(&act.sa_mask);
        act.sa_flags = SA_SIGINFO;  // need si_addr; no SA_NODEFER (SIGBUS
                                    // stays blocked while handling, and
                                    // sigsetjmp(.,1)+siglongjmp restores
                                    // the mask on the recovery path).
        if (::sigaction(SIGBUS, &act, &g_previous_sigbus) != 0) {
            throw PdfToolkitException(ErrorCode::Internal,
                                      "sigaction(SIGBUS) failed");
        }
    });
}

// Registers [begin, end) with the guard registry. False when the table is
// full. Must be called with the handler already installed.
bool guard_register(const std::uint8_t* begin, const std::uint8_t* end) noexcept {
    const std::lock_guard<std::mutex> lock(g_guard_mutex);
    for (GuardSlot& slot : g_guard_slots) {
        if (slot.begin != nullptr) {
            continue;
        }
        const std::uint64_t version =
            slot.version.load(std::memory_order_relaxed);
        slot.version.store(version + 1, std::memory_order_relaxed);  // odd
        slot.begin = begin;
        slot.end = end;
        slot.version.store(version + 2, std::memory_order_release);
        return true;
    }
    return false;
}

void guard_unregister(const std::uint8_t* begin) noexcept {
    const std::lock_guard<std::mutex> lock(g_guard_mutex);
    for (GuardSlot& slot : g_guard_slots) {
        if (slot.begin != begin) {
            continue;
        }
        const std::uint64_t version =
            slot.version.load(std::memory_order_relaxed);
        slot.version.store(version + 1, std::memory_order_relaxed);  // odd
        slot.begin = nullptr;
        slot.end = nullptr;
        slot.version.store(version + 2, std::memory_order_release);
        return;
    }
}

// Maps errno from ::open to the constructor contract in mmap.hpp.
[[noreturn]] void throw_open_error(int err, const char* path) {
    if (err == ENOENT || err == ENOTDIR) {
        // No such path (or a path component is not a directory): the
        // document cannot be found.
        throw PdfToolkitException(ErrorCode::DocumentNotFound, path);
    }
    // EACCES and every other "cannot open for reading" (ELOOP, EMFILE,
    // ENAMETOOLONG, ...) surface as UnreadablePdf per the header contract.
    throw PdfToolkitException(ErrorCode::UnreadablePdf, std::strerror(err));
}

}  // namespace

namespace detail {

GuardInstall::GuardInstall(void* buf) noexcept {
    // Publishes the thread-local recovery point consumed by the SIGBUS
    // handler (see sigbus_handler for the consume side).
    t_recovery = static_cast<sigjmp_buf*>(buf);
}

GuardInstall::~GuardInstall() { t_recovery = nullptr; }

}  // namespace detail

MmapHandle::MmapHandle(const char* path) {
    if (path == nullptr || *path == '\0') {
        throw PdfToolkitException(ErrorCode::InvalidArgument,
                                  "path is null or empty");
    }
    path_ = path;

    fd_ = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) {
        throw_open_error(errno, path);
    }

    struct stat st;
    if (::fstat(fd_, &st) != 0) {
        const int err = errno;
        ::close(fd_);
        fd_ = -1;
        throw PdfToolkitException(ErrorCode::Internal, std::strerror(err));
    }
    if (!S_ISREG(st.st_mode)) {
        ::close(fd_);
        fd_ = -1;
        throw PdfToolkitException(ErrorCode::UnreadablePdf,
                                  "not a regular file");
    }
    if (st.st_size < 0 ||
        static_cast<std::uintmax_t>(st.st_size) >
            static_cast<std::uintmax_t>(
                std::numeric_limits<std::size_t>::max())) {
        ::close(fd_);
        fd_ = -1;
        throw PdfToolkitException(ErrorCode::UnreadablePdf,
                                  "file too large for this platform");
    }
    length_ = static_cast<std::size_t>(st.st_size);

    if (length_ == 0) {
        // Valid empty handle: no mapping, no guard slot — and the fd is
        // no longer needed (nothing is mapped).
        ::close(fd_);
        fd_ = -1;
        return;
    }

    void* addr = ::mmap(nullptr, length_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (addr == MAP_FAILED) {
        const int err = errno;
        ::close(fd_);
        fd_ = -1;
        throw PdfToolkitException(ErrorCode::Internal, std::strerror(err));
    }
    data_ = static_cast<const std::uint8_t*>(addr);

    // The mapping exists but is unregistered until the handler is in
    // place and the slot is published; failure of either rolls back.
    try {
        install_sigbus_handler_once();
    } catch (...) {
        ::munmap(const_cast<std::uint8_t*>(data_), length_);
        data_ = nullptr;
        length_ = 0;
        ::close(fd_);
        fd_ = -1;
        throw;
    }
    if (!guard_register(data_, data_ + length_)) {
        ::munmap(const_cast<std::uint8_t*>(data_), length_);
        data_ = nullptr;
        length_ = 0;
        ::close(fd_);
        fd_ = -1;
        throw PdfToolkitException(
            ErrorCode::Internal,
            "guard table exhausted (128 live mappings)");
    }
}

MmapHandle::~MmapHandle() { destroy(); }

MmapHandle::MmapHandle(MmapHandle&& other) noexcept
    : path_(std::move(other.path_)),
      data_(other.data_),
      length_(other.length_),
      fd_(other.fd_) {
    other.data_ = nullptr;
    other.length_ = 0;
    other.fd_ = -1;
}

MmapHandle& MmapHandle::operator=(MmapHandle&& other) noexcept {
    if (this != &other) {
        destroy();
        path_ = std::move(other.path_);
        data_ = other.data_;
        length_ = other.length_;
        fd_ = other.fd_;
        other.data_ = nullptr;
        other.length_ = 0;
        other.fd_ = -1;
    }
    return *this;
}

bool MmapHandle::valid() const noexcept { return !path_.empty(); }

bool MmapHandle::owns(const void* p) const noexcept {
    if (data_ == nullptr || p == nullptr) {
        return false;
    }
    return range_contains(data_, data_ + length_, p);
}

bool MmapHandle::advise(Advice advice) noexcept {
    if (data_ == nullptr) {
        return false;  // unmapped (empty or moved-from) handle
    }
    const int native =
        advice == Advice::WillNeed ? POSIX_MADV_WILLNEED : POSIX_MADV_RANDOM;
    return ::posix_madvise(const_cast<std::uint8_t*>(data_), length_,
                           native) == 0;
}

void MmapHandle::destroy() noexcept {
    if (data_ != nullptr) {
        guard_unregister(data_);
        ::munmap(const_cast<std::uint8_t*>(data_), length_);
        data_ = nullptr;
        length_ = 0;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    path_.clear();  // valid() is false from here on
}

}  // namespace pdftoolkit::memory

#else  // _WIN32 ---------------------------------------------------------------

#include <windows.h>

namespace pdftoolkit::memory {

namespace {

// Win32 compile-only path (U-011): never executed in this (GCC/Linux)
// reference environment, but kept correct by construction so the first
// Windows CI run (U-008 precedent) validates rather than discovers it.

[[noreturn]] void throw_win32_open_error(DWORD err, const char* path) {
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
        throw PdfToolkitException(ErrorCode::DocumentNotFound, path);
    }
    throw PdfToolkitException(ErrorCode::UnreadablePdf,
                              "CreateFileA failed");
}

}  // namespace

namespace detail {

// guarded() runs f unguarded on Windows (see mmap.hpp, U-011), so the
// recovery-point publisher is an empty shell there.
GuardInstall::GuardInstall(void* /*buf*/) noexcept {}
GuardInstall::~GuardInstall() {}

}  // namespace detail

MmapHandle::MmapHandle(const char* path) {
    if (path == nullptr || *path == '\0') {
        throw PdfToolkitException(ErrorCode::InvalidArgument,
                                  "path is null or empty");
    }
    path_ = path;

    file_ = ::CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        file_ = nullptr;
        throw_win32_open_error(::GetLastError(), path);
    }

    LARGE_INTEGER size{};
    if (::GetFileSizeEx(file_, &size) == 0) {
        ::CloseHandle(file_);
        file_ = nullptr;
        throw PdfToolkitException(ErrorCode::Internal,
                                  "GetFileSizeEx failed");
    }
    if (size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) >
            static_cast<unsigned long long>(
                std::numeric_limits<std::size_t>::max())) {
        ::CloseHandle(file_);
        file_ = nullptr;
        throw PdfToolkitException(ErrorCode::UnreadablePdf,
                                  "file too large for this platform");
    }
    length_ = static_cast<std::size_t>(size.QuadPart);

    if (length_ == 0) {
        // Valid empty handle; CreateFileMappingA cannot represent a
        // zero-length file (ERROR_FILE_INVALID), so map nothing.
        ::CloseHandle(file_);
        file_ = nullptr;
        return;
    }

    section_ = ::CreateFileMappingA(file_, nullptr, PAGE_READONLY, 0, 0,
                                    nullptr);
    if (section_ == nullptr) {
        ::CloseHandle(file_);
        file_ = nullptr;
        throw PdfToolkitException(ErrorCode::Internal,
                                  "CreateFileMappingA failed");
    }
    data_ = static_cast<const std::uint8_t*>(
        ::MapViewOfFile(section_, FILE_MAP_READ, 0, 0, 0));
    if (data_ == nullptr) {
        ::CloseHandle(section_);
        section_ = nullptr;
        ::CloseHandle(file_);
        file_ = nullptr;
        throw PdfToolkitException(ErrorCode::Internal,
                                  "MapViewOfFile failed");
    }
}

MmapHandle::~MmapHandle() { destroy(); }

MmapHandle::MmapHandle(MmapHandle&& other) noexcept
    : path_(std::move(other.path_)),
      data_(other.data_),
      length_(other.length_),
      file_(other.file_),
      section_(other.section_) {
    other.data_ = nullptr;
    other.length_ = 0;
    other.file_ = nullptr;
    other.section_ = nullptr;
}

MmapHandle& MmapHandle::operator=(MmapHandle&& other) noexcept {
    if (this != &other) {
        destroy();
        path_ = std::move(other.path_);
        data_ = other.data_;
        length_ = other.length_;
        file_ = other.file_;
        section_ = other.section_;
        other.data_ = nullptr;
        other.length_ = 0;
        other.file_ = nullptr;
        other.section_ = nullptr;
    }
    return *this;
}

bool MmapHandle::valid() const noexcept { return !path_.empty(); }

bool MmapHandle::owns(const void* p) const noexcept {
    if (data_ == nullptr || p == nullptr) {
        return false;
    }
    const auto f = reinterpret_cast<std::uintptr_t>(p);
    return f >= reinterpret_cast<std::uintptr_t>(data_) &&
           f < reinterpret_cast<std::uintptr_t>(data_ + length_);
}

bool MmapHandle::advise(Advice /*advice*/) noexcept {
    // No posix_madvise equivalent is wired yet (PrefetchVirtualMemory is
    // the candidate); advisory hints are best-effort by contract, so a
    // plain "not delivered" is honest. See U-011.
    return false;
}

void MmapHandle::destroy() noexcept {
    if (data_ != nullptr) {
        ::UnmapViewOfFile(data_);
        data_ = nullptr;
        length_ = 0;
    }
    if (section_ != nullptr) {
        ::CloseHandle(section_);
        section_ = nullptr;
    }
    if (file_ != nullptr) {
        ::CloseHandle(file_);
        file_ = nullptr;
    }
    path_.clear();
}

}  // namespace pdftoolkit::memory

#endif  // _WIN32
