#include "pdftoolkit/errors.hpp"

#include <string>

namespace pdftoolkit {

PdfToolkitException::PdfToolkitException(ErrorCode code, const char* detail)
    : std::runtime_error(detail == nullptr
                              ? std::string(error_message(code))
                              : std::string(error_message(code)) + ": " + detail),
      code_(code) {}

const char* error_message(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok:               return "ok";
        case ErrorCode::InvalidArgument:  return "invalid argument";
        case ErrorCode::BufferTooSmall:   return "buffer too small";
        case ErrorCode::IoTruncated:      return "file truncated under memory map";
        case ErrorCode::OutputConflict:   return "output path conflicts with an input document";
        case ErrorCode::UnreadablePdf:    return "unreadable PDF";
        case ErrorCode::DocumentNotFound: return "document not found";
        case ErrorCode::ContentNotFound:  return "content not found";
        case ErrorCode::InvalidPageRange: return "invalid page range";
        case ErrorCode::NotImplemented:   return "not implemented yet (see docs/recovery/task-registry.md)";
        case ErrorCode::Internal:         return "internal engine error";
    }
    return "unknown error code";
}

}  // namespace pdftoolkit
