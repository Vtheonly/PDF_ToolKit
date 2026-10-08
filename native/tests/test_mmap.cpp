// MmapHandle unit tests (audit issue-1/task-1.1).
//
// The acceptance criterion is the truncation test: accessing a mapping
// after the underlying file was shrunk must surface
// PdfToolkitException(IoTruncated) through guarded() — a controlled
// exception, never a process crash.
//
// The three "honesty" tests fork: they assert that faults the guard is
// NOT contracted to swallow (no recovery point, foreign mapping, dead
// handle) still kill the process with the default disposition. These
// must run in a child because the parent's test process must survive.
// Children are written allocation-free after fork() so they also behave
// under the sanitizers' runtimes (TSan in particular dislikes
// post-fork allocation on inherited runtime locks).

#include "test_harness.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <signal.h>
#include <string>
#include <vector>

#include "pdftoolkit/errors.hpp"
#include "pdftoolkit/memory/mmap.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using pdftoolkit::ErrorCode;
using pdftoolkit::PdfToolkitException;
using pdftoolkit::memory::Advice;
using pdftoolkit::memory::MmapHandle;

// Deterministic LCG so every test byte is predictable.
std::vector<std::uint8_t> make_pattern(std::size_t bytes, std::uint32_t seed) {
    std::vector<std::uint8_t> content(bytes);
    std::uint32_t x = seed;
    for (std::size_t i = 0; i < bytes; ++i) {
        x = x * 1664525u + 1013904223u;
        content[i] = static_cast<std::uint8_t>(x >> 24);
    }
    return content;
}

class TempFile {
public:
    TempFile(std::size_t bytes, std::uint32_t seed)
        : content_(make_pattern(bytes, seed)),
          path_(std::filesystem::temp_directory_path() /
                ("pdtk_test_mmap_" + std::to_string(::getpid()) + "_XXXXXX")) {
        std::string tmpl = path_.string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        const int fd = ::mkstemp(buf.data());
        PDTK_ASSERT(fd >= 0);
        path_ = buf.data();
        std::size_t written = 0;
        while (written < content_.size()) {
            const ssize_t n = ::write(fd, content_.data() + written,
                                      content_.size() - written);
            PDTK_ASSERT(n > 0);
            written += static_cast<std::size_t>(n);
        }
        PDTK_ASSERT_EQ(::close(fd), 0);
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const char* c_str() const { return path_.c_str(); }
    const std::vector<std::uint8_t>& content() const { return content_; }
    std::size_t size() const { return content_.size(); }

private:
    std::vector<std::uint8_t> content_;
    std::filesystem::path path_;
};

long page_size() {
    const long page = ::sysconf(_SC_PAGESIZE);
    PDTK_ASSERT(page > 0);
    return page;
}

// ---------------------------------------------------------------------------
// Construction, mapping and error mapping
// ---------------------------------------------------------------------------

PDTK_TEST(maps_file_contents_verbatim) {
    TempFile file(8192, 0xA5A5A5u);
    MmapHandle handle(file.c_str());
    PDTK_ASSERT(handle.valid());
    PDTK_ASSERT_EQ(handle.size(), file.size());
    const auto bytes = handle.bytes();
    PDTK_ASSERT_EQ(bytes.size(), file.size());
    PDTK_ASSERT(std::memcmp(bytes.data(), file.content().data(),
                            file.size()) == 0);
    PDTK_ASSERT(handle.owns(bytes.data()));
    PDTK_ASSERT(!handle.owns(bytes.data() + file.size()));
    PDTK_ASSERT(!handle.owns(nullptr));
}

PDTK_TEST(empty_file_is_valid_with_empty_span) {
    TempFile file(0, 1u);
    MmapHandle handle(file.c_str());
    PDTK_ASSERT(handle.valid());  // a zero-length file is a valid mapping
    PDTK_ASSERT_EQ(handle.size(), 0u);
    PDTK_ASSERT(handle.bytes().empty());
    PDTK_ASSERT(handle.bytes().data() == nullptr);
    PDTK_ASSERT(!handle.owns(file.c_str()));  // nothing is ever owned
    // No mapping exists: advice cannot be delivered, and by contract it
    // reports that instead of throwing.
    PDTK_ASSERT(!handle.advise(Advice::WillNeed));
    PDTK_ASSERT(!handle.advise(Advice::Random));
}

PDTK_TEST(null_and_empty_path_is_invalid_argument) {
    bool threw = false;
    try {
        MmapHandle handle(nullptr);
        (void)handle;
    } catch (const PdfToolkitException& e) {
        threw = true;
        PDTK_ASSERT_EQ(e.code(), ErrorCode::InvalidArgument);
    }
    PDTK_ASSERT(threw);

    threw = false;
    try {
        MmapHandle handle("");
        (void)handle;
    } catch (const PdfToolkitException& e) {
        threw = true;
        PDTK_ASSERT_EQ(e.code(), ErrorCode::InvalidArgument);
    }
    PDTK_ASSERT(threw);
}

PDTK_TEST(missing_file_is_document_not_found) {
    for (const char* missing :
         {"/nonexistent/pdtk/definitely-missing.pdf",   // ENOENT
          "/nonexistent/pdtk/dir.pdf/child.pdf"}) {     // ENOTDIR
        bool threw = false;
        try {
            MmapHandle handle(missing);
            (void)handle;
        } catch (const PdfToolkitException& e) {
            threw = true;
            PDTK_ASSERT_EQ(e.code(), ErrorCode::DocumentNotFound);
        }
        PDTK_ASSERT(threw);
    }
}

PDTK_TEST(directory_is_unreadable_pdf) {
    bool threw = false;
    try {
        MmapHandle handle(".");
        (void)handle;
    } catch (const PdfToolkitException& e) {
        threw = true;
        PDTK_ASSERT_EQ(e.code(), ErrorCode::UnreadablePdf);
    }
    PDTK_ASSERT(threw);
}

PDTK_TEST(unreadable_file_is_unreadable_pdf) {
    if (::geteuid() == 0) {
        // Root ignores permission bits; the EACCES path is untestable
        // (and would silently pass) when running as root.
        std::printf("[ SKIP ] unreadable_file_is_unreadable_pdf (euid 0)\n");
        return;
    }
    TempFile file(4096, 2u);
    PDTK_ASSERT_EQ(::chmod(file.c_str(), 0000), 0);
    bool threw = false;
    try {
        MmapHandle handle(file.c_str());
        (void)handle;
    } catch (const PdfToolkitException& e) {
        threw = true;
        PDTK_ASSERT_EQ(e.code(), ErrorCode::UnreadablePdf);
    }
    PDTK_ASSERT(threw);
}

// ---------------------------------------------------------------------------
// Move semantics
// ---------------------------------------------------------------------------

PDTK_TEST(move_constructor_transfers_ownership_without_relocation) {
    TempFile file(4096, 3u);
    MmapHandle a(file.c_str());
    const std::uint8_t* const old_data = a.bytes().data();
    const std::size_t old_size = a.size();

    MmapHandle b(std::move(a));
    PDTK_ASSERT(!a.valid());
    PDTK_ASSERT_EQ(a.size(), 0u);
    PDTK_ASSERT(b.valid());
    PDTK_ASSERT_EQ(b.size(), old_size);
    PDTK_ASSERT_EQ(b.bytes().data(), old_data);  // address never moves
    PDTK_ASSERT_EQ(b.bytes()[0], file.content()[0]);
    PDTK_ASSERT(!a.owns(old_data));
    PDTK_ASSERT(b.owns(old_data));
}

PDTK_TEST(move_assignment_releases_target_and_takes_source) {
    TempFile left(4096, 4u);
    TempFile right(8192, 5u);
    MmapHandle a(left.c_str());
    MmapHandle b(right.c_str());
    const std::uint8_t* const right_data = b.bytes().data();

    a = std::move(b);
    PDTK_ASSERT(!b.valid());
    PDTK_ASSERT(a.valid());
    PDTK_ASSERT_EQ(a.size(), right.size());
    PDTK_ASSERT_EQ(a.bytes().data(), right_data);
    PDTK_ASSERT(std::memcmp(a.bytes().data(), right.content().data(),
                            right.size()) == 0);

    // Self-assignment is a documented no-op-safe path. The alias keeps
    // GCC 14's -Wself-move (correct for accidental self-moves) quiet —
    // the move IS the case under test.
    MmapHandle& self = a;
    a = std::move(self);
    PDTK_ASSERT(a.valid());
    PDTK_ASSERT_EQ(a.size(), right.size());
}

// ---------------------------------------------------------------------------
// Kernel advice
// ---------------------------------------------------------------------------

PDTK_TEST(advise_is_advisory_and_never_throws) {
    TempFile file(64 * 1024, 6u);
    MmapHandle handle(file.c_str());
    // posix_madvise on a live mapping must succeed on POSIX; either way
    // the contract is "never throws".
    PDTK_ASSERT(handle.advise(Advice::WillNeed));
    PDTK_ASSERT(handle.advise(Advice::Random));
    PDTK_ASSERT(handle.advise(Advice::WillNeed));  // repeatable

    MmapHandle moved = std::move(handle);
    PDTK_ASSERT(moved.advise(Advice::WillNeed));   // new owner: live mapping
    PDTK_ASSERT(!handle.advise(Advice::WillNeed));  // moved-from: no mapping
}

// ---------------------------------------------------------------------------
// Guarded access — the audit task-1.1 acceptance criterion
// ---------------------------------------------------------------------------

PDTK_TEST(guard_passes_through_return_values) {
    TempFile file(4096, 7u);
    MmapHandle handle(file.c_str());
    const std::uint8_t got = handle.guarded(
        [&handle] { return handle.bytes()[3]; });
    PDTK_ASSERT_EQ(got, file.content()[3]);
}

PDTK_TEST(guard_converts_truncation_to_typed_exception) {
    const long page = page_size();
    TempFile file(static_cast<std::size_t>(page) * 16, 0xC0FFEEu);
    MmapHandle handle(file.c_str());
    PDTK_ASSERT_EQ(handle.size(), static_cast<std::size_t>(page) * 16);

    // Shrink the file behind the mapping's back: every page beyond the
    // new EOF loses its backing; touching one raises SIGBUS.
    const int fd = ::open(file.c_str(), O_WRONLY);
    PDTK_ASSERT(fd >= 0);
    PDTK_ASSERT_EQ(::ftruncate(fd, page), 0);  // keep exactly one page
    PDTK_ASSERT_EQ(::close(fd), 0);

    // Bytes below the truncation point stay readable under the guard...
    handle.guarded([&handle, &file, page] {
        PDTK_ASSERT_EQ(handle.bytes()[0], file.content()[0]);
        PDTK_ASSERT_EQ(handle.bytes()[static_cast<std::size_t>(page) - 1],
                       file.content()[static_cast<std::size_t>(page) - 1]);
    });

    // ...and the first byte beyond EOF converts SIGBUS into the typed
    // error instead of killing the process — the acceptance criterion.
    for (int round = 0; round < 2; ++round) {  // the guard re-arms per call
        bool threw = false;
        try {
            handle.guarded([&handle, page] {
                volatile const std::uint8_t* p = handle.bytes().data();
                const std::uint8_t sink =
                    p[static_cast<std::size_t>(page)];  // beyond new EOF
                (void)sink;
            });
            PDTK_ASSERT(false && "guarded access beyond EOF must throw");
        } catch (const PdfToolkitException& e) {
            threw = true;
            PDTK_ASSERT_EQ(e.code(), ErrorCode::IoTruncated);
        }
        PDTK_ASSERT(threw);
    }
}

// ---------------------------------------------------------------------------
// Honesty contract — faults the guard must NOT swallow (forked children)
// ---------------------------------------------------------------------------

// True when a sanitizer runtime may own the previous SIGBUS disposition
// (its re-raise path reports and Dies with an exit code instead of the
// default death-by-signal).
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitizersActive = true;
#else
constexpr bool kSanitizersActive = false;
#endif

// Asserts the child did not survive cleanly. Without sanitizers the
// death must be exactly SIGBUS (or SIGSEGV where noted) — the default
// disposition, as if no guard machinery existed.
void assert_child_died(int status, int expected_signal) {
    if (kSanitizersActive) {
        // The sanitizer's own handler chains in and exits with its
        // report code; "exited 0" is the only failure mode that would
        // mean the fault was wrongly swallowed.
        PDTK_ASSERT(!(WIFEXITED(status) && WEXITSTATUS(status) == 0));
        return;
    }
    PDTK_ASSERT(WIFSIGNALED(status));
    PDTK_ASSERT_EQ(WTERMSIG(status), expected_signal);
}

PDTK_TEST(honesty_fault_without_recovery_point_dies) {
    const long page = page_size();
    TempFile file(static_cast<std::size_t>(page) * 8, 8u);
    // Everything the child needs is prepared pre-fork: the child itself
    // performs no allocation (sanitizer-run-time fork safety).
    MmapHandle handle(file.c_str());
    const int fd = ::open(file.c_str(), O_WRONLY);
    PDTK_ASSERT(fd >= 0);
    PDTK_ASSERT_EQ(::ftruncate(fd, page), 0);
    PDTK_ASSERT_EQ(::close(fd), 0);
    volatile const std::uint8_t* const fault_page =
        handle.bytes().data() + static_cast<std::size_t>(page) * 4;

    const pid_t pid = ::fork();
    PDTK_ASSERT(pid >= 0);
    if (pid == 0) {
        // No guarded() is active (thread-local recovery is null): the
        // handler must restore the default disposition and re-raise.
        const std::uint8_t sink = *fault_page;
        (void)sink;
        ::_exit(0);  // reached only if the fault was wrongly swallowed
    }
    int status = 0;
    PDTK_ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    assert_child_died(status, SIGBUS);
}

PDTK_TEST(honesty_foreign_fault_under_guard_dies) {
    const long page = page_size();
    // A raw mmap over a second file — deliberately NOT registered with
    // the guard registry (MmapHandle never saw it).
    TempFile foreign(static_cast<std::size_t>(page) * 8, 9u);
    const int fd = ::open(foreign.c_str(), O_RDONLY);
    PDTK_ASSERT(fd >= 0);
    void* foreign_map =
        ::mmap(nullptr, static_cast<std::size_t>(page) * 8, PROT_READ,
               MAP_PRIVATE, fd, 0);
    PDTK_ASSERT(foreign_map != MAP_FAILED);
    const int wfd = ::open(foreign.c_str(), O_WRONLY);
    PDTK_ASSERT(wfd >= 0);
    PDTK_ASSERT_EQ(::ftruncate(wfd, page), 0);
    PDTK_ASSERT_EQ(::close(wfd), 0);
    PDTK_ASSERT_EQ(::close(fd), 0);

    TempFile guardfile(4096, 10u);
    MmapHandle handle(guardfile.c_str());  // provides guarded(), stays live
    volatile const std::uint8_t* const fault_page =
        static_cast<const std::uint8_t*>(foreign_map) +
        static_cast<std::size_t>(page) * 4;

    const pid_t pid = ::fork();
    PDTK_ASSERT(pid >= 0);
    if (pid == 0) {
        // Recovery point IS active, but the fault address is foreign
        // (unregistered mapping): must still die with the default
        // disposition — protection is opt-in per mapping, never blanket.
        handle.guarded([&fault_page] {
            const std::uint8_t sink = *fault_page;
            (void)sink;
        });
        ::_exit(0);  // reached only if wrongly swallowed
    }
    int status = 0;
    PDTK_ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    PDTK_ASSERT_EQ(::munmap(foreign_map, static_cast<std::size_t>(page) * 8),
                   0);
    assert_child_died(status, SIGBUS);
}

PDTK_TEST(honesty_handle_destroyed_then_fault_dies) {
    const long page = page_size();
    TempFile dead(static_cast<std::size_t>(page) * 8, 11u);
    auto dead_handle = std::make_unique<MmapHandle>(dead.c_str());
    // All allocation-bearing setup happens BEFORE the unmap, so the
    // freed range cannot be recycled by the allocator before the fork.
    TempFile guardfile(4096, 12u);
    MmapHandle handle(guardfile.c_str());  // provides guarded(), stays live
    volatile const std::uint8_t* const stale =
        dead_handle->bytes().data();  // dangling once released
    dead_handle.reset();              // unregister + munmap

    const pid_t pid = ::fork();
    PDTK_ASSERT(pid >= 0);
    if (pid == 0) {
        // The stale span points into unmapped memory: the fault (SIGSEGV
        // — the mapping is gone entirely) must kill the process; the
        // guard machinery was never told to own it anymore.
        handle.guarded([stale] {
            const std::uint8_t sink = *stale;
            (void)sink;
        });
        ::_exit(0);
    }
    int status = 0;
    PDTK_ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    assert_child_died(status, SIGSEGV);
}

// ---------------------------------------------------------------------------
// Guard registry capacity and slot recycling
// ---------------------------------------------------------------------------

PDTK_TEST(guard_table_holds_128_mappings_and_reports_exhaustion) {
    TempFile file(4096, 13u);
    std::vector<MmapHandle> handles;
    handles.reserve(129);
    for (int i = 0; i < 128; ++i) {
        handles.emplace_back(file.c_str());  // 128 distinct mappings
    }
    bool threw = false;
    try {
        handles.emplace_back(file.c_str());  // 129th: table full
    } catch (const PdfToolkitException& e) {
        threw = true;
        PDTK_ASSERT_EQ(e.code(), ErrorCode::Internal);
    }
    PDTK_ASSERT(threw);

    handles.clear();  // releases every slot
    // Slot recycling: a fresh mapping must register again and read fine.
    MmapHandle recycled(file.c_str());
    PDTK_ASSERT(recycled.valid());
    PDTK_ASSERT_EQ(recycled.bytes()[0], file.content()[0]);
}

}  // namespace

PDTK_TEST_MAIN()
