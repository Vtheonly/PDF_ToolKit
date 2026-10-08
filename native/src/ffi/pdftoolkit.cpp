// pdftoolkit-ffi — panic-proof C-ABI export layer (audit task 7.1 shape,
// scaffolded early by task 0.1 so all four build targets exist).
//
// Hard rules enforced here:
//   * no C++ exception crosses the boundary (every body is wrapped in
//     try/catch(...) -> integer status);
//   * output parameters are only written on success;
//   * stubs return PDTK_ERR_NOT_IMPLEMENTED — success is never faked.

#include "pdftoolkit/pdftoolkit.h"

#include <cstring>
#include <filesystem>
#include <new>
#include <string>
#include <unordered_map>

#include "pdftoolkit/errors.hpp"
#include "pdftoolkit/version.hpp"

namespace {

using pdftoolkit::ErrorCode;

struct EngineState {
    // Scaffolding registry: path -> stable id. The lock-free atomic
    // registry (audit task 6.1) replaces this when it lands.
    std::unordered_map<std::string, uint32_t> doc_ids;
    uint32_t next_doc_id = 1;
};

int32_t to_status(ErrorCode code) noexcept {
    return static_cast<int32_t>(code);
}

}  // namespace

struct EngineHandle final {
    EngineState state;
};

extern "C" {

int32_t pdftoolkit_engine_create(EngineHandle** out_handle) noexcept {
    try {
        if (out_handle == nullptr) {
            return to_status(ErrorCode::InvalidArgument);
        }
        *out_handle = new EngineHandle();
        return PDTK_OK;
    } catch (...) {
        return to_status(ErrorCode::Internal);
    }
}

int32_t pdftoolkit_engine_destroy(EngineHandle* handle) noexcept {
    try {
        if (handle == nullptr) {
            return to_status(ErrorCode::InvalidArgument);
        }
        delete handle;
        return PDTK_OK;
    } catch (...) {
        return to_status(ErrorCode::Internal);
    }
}

int32_t pdftoolkit_version(char* out_buf, uint32_t buf_len) noexcept {
    try {
        if (out_buf == nullptr || buf_len == 0) {
            return to_status(ErrorCode::InvalidArgument);
        }
        const char* version = pdftoolkit::native_version();
        const std::size_t length = std::strlen(version);  // excludes NUL
        if (length + 1 > static_cast<std::size_t>(buf_len)) {
            return to_status(ErrorCode::BufferTooSmall);
        }
        std::memcpy(out_buf, version, length + 1);
        return PDTK_OK;
    } catch (...) {
        return to_status(ErrorCode::Internal);
    }
}

int32_t pdftoolkit_register_document(EngineHandle* handle,
                                     const char* path,
                                     uint32_t* out_doc_id) noexcept {
    try {
        if (handle == nullptr || path == nullptr || out_doc_id == nullptr) {
            return to_status(ErrorCode::InvalidArgument);
        }
        const std::filesystem::path fs_path(path);
        std::error_code ec;
        if (!std::filesystem::exists(fs_path, ec)) {
            return to_status(ErrorCode::DocumentNotFound);
        }
        // Stable ids: registering the same path twice yields the same id.
        const auto [entry, inserted] =
            handle->state.doc_ids.try_emplace(fs_path.string(), handle->state.next_doc_id);
        if (inserted) {
            ++handle->state.next_doc_id;
        }
        *out_doc_id = entry->second;
        return PDTK_OK;
    } catch (...) {
        return to_status(ErrorCode::Internal);
    }
}

int32_t pdftoolkit_search_wand(EngineHandle* handle,
                               const char* query,
                               uint32_t top_k,
                               uint64_t timeout_ms,
                               FfiResultSlice* out_slice) noexcept {
    try {
        if (out_slice != nullptr) {
            out_slice->data = nullptr;
            out_slice->length = 0;
        }
        if (handle == nullptr || query == nullptr || out_slice == nullptr) {
            return to_status(ErrorCode::InvalidArgument);
        }
        // Honest stub: the WAND engine arrives with audit task 4.2.
        (void)top_k;
        (void)timeout_ms;
        return to_status(ErrorCode::NotImplemented);
    } catch (...) {
        return to_status(ErrorCode::Internal);
    }
}

void pdftoolkit_result_slice_free(FfiResultSlice* slice) noexcept {
    if (slice == nullptr) {
        return;
    }
    slice->data = nullptr;
    slice->length = 0;
}

}  // extern "C"
