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
#include <memory>
#include <new>
#include <string>
#include <unordered_map>

#include "pdftoolkit/errors.hpp"
#include "pdftoolkit/memory/mmap.hpp"
#include "pdftoolkit/version.hpp"

namespace {

using pdftoolkit::ErrorCode;
using pdftoolkit::memory::Advice;
using pdftoolkit::memory::MmapHandle;

// One registered document: a stable id plus the zero-copy mapping that
// arrived with audit task 1.1. shared_ptr per the audit's Phase-1
// architecture diagram — later phases (parser, slab) share this mapping
// rather than re-reading the file.
struct DocEntry {
    uint32_t id;
    std::shared_ptr<MmapHandle> mapping;
};

struct EngineState {
    // Scaffolding registry: path -> stable id + live mapping. The
    // lock-free atomic registry (audit task 6.1) replaces this when it
    // lands.
    std::unordered_map<std::string, DocEntry> docs;
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

        // Stable ids AND a stable mapping: re-registering a path returns
        // the id of the first registration and keeps the first mapping
        // (documents are treated as immutable corpus files; the file is
        // only re-mapped if this engine handle never saw the path).
        const auto existing = handle->state.docs.find(fs_path.string());
        if (existing != handle->state.docs.end()) {
            *out_doc_id = existing->second.id;
            return PDTK_OK;
        }

        // Audit task 1.1 wiring: registration maps the file zero-copy
        // (MmapHandle) instead of merely checking existence. Constructor
        // failures map to the C-ABI status codes (DocumentNotFound,
        // UnreadablePdf, Internal) via the typed exception.
        auto mapping = std::make_shared<MmapHandle>(path);
        // Sequential corpus ingestion (audit task 1.1 step 3): ask the
        // kernel for read-ahead. Advisory only — a refusal is not an
        // error.
        (void)mapping->advise(Advice::WillNeed);

        const uint32_t id = handle->state.next_doc_id++;
        handle->state.docs.emplace(fs_path.string(),
                                   DocEntry{id, std::move(mapping)});
        *out_doc_id = id;
        return PDTK_OK;
    } catch (const pdftoolkit::PdfToolkitException& e) {
        return to_status(e.code());
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
