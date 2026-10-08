#pragma once

#include <cstdint>

// The C-ABI status codes are the single source of truth for error codes;
// the C++ enum mirrors them one-to-one (same values, by construction).
#include "pdftoolkit/pdftoolkit.h"

namespace pdftoolkit {

/// Typed error codes used inside the C++ core. Values intentionally match
/// the C-ABI `PDTK_*` codes so the FFI layer can translate with a plain
/// static_cast (audit task 7.1).
enum class ErrorCode : int32_t {
    Ok = PDTK_OK,
    InvalidArgument = PDTK_ERR_INVALID_ARGUMENT,
    BufferTooSmall = PDTK_ERR_BUFFER_TOO_SMALL,
    IoTruncated = PDTK_ERR_IO_TRUNCATED,
    OutputConflict = PDTK_ERR_OUTPUT_CONFLICT,
    UnreadablePdf = PDTK_ERR_UNREADABLE_PDF,
    DocumentNotFound = PDTK_ERR_DOCUMENT_NOT_FOUND,
    ContentNotFound = PDTK_ERR_CONTENT_NOT_FOUND,
    InvalidPageRange = PDTK_ERR_INVALID_PAGE_RANGE,
    NotImplemented = PDTK_ERR_NOT_IMPLEMENTED,
    Internal = PDTK_ERR_INTERNAL,
};

/// Stable, human-readable message for every error code (never null).
[[nodiscard]] const char* error_message(ErrorCode code) noexcept;

}  // namespace pdftoolkit
