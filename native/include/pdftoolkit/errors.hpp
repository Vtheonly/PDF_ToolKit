#pragma once

#include <cstdint>
#include <stdexcept>

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

/// Typed native-core exception. The audit (task 1.1) requires SIGBUS
/// truncation faults to surface as PdfToolkitException(IoTruncated);
/// later phases reuse it for every typed failure crossing internal
/// (non-C-ABI) boundaries. The C-ABI layer still translates it to a
/// status code — it never crosses extern "C" (audit task 7.1).
class PdfToolkitException : public std::runtime_error {
public:
    /// `detail` (when non-null) is appended to the standard message —
    /// used for errno/strerror context on I/O failures.
    PdfToolkitException(ErrorCode code, const char* detail = nullptr);

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

}  // namespace pdftoolkit
