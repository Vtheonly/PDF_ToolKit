#include "pdftoolkit/memory/arena.hpp"

#include <cstdlib>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace pdftoolkit::memory {

unsigned char* BumpArena::allocate(std::size_t bytes) noexcept {
    if (bytes == 0) {
        bytes = kAlignment;  // keep a well-formed, freeable allocation
    }
#if defined(_MSC_VER)
    // NOTE: MSVC path not locally verifiable (reference env is GCC-only,
    // see docs/recovery/unknowns.md U-008); compiled and tested in CI.
    return static_cast<unsigned char*>(_aligned_malloc(bytes, kAlignment));
#else
    return static_cast<unsigned char*>(std::aligned_alloc(kAlignment, bytes));
#endif
}

void BumpArena::release(unsigned char* buffer) noexcept {
    if (buffer == nullptr) {
        return;
    }
#if defined(_MSC_VER)
    _aligned_free(buffer);
#else
    std::free(buffer);
#endif
}

}  // namespace pdftoolkit::memory
