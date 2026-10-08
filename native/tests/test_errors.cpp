// Unit tests for the ErrorCode taxonomy and its C-ABI correspondence.

#include "test_harness.hpp"

#include <cstring>

#include "pdftoolkit/errors.hpp"
#include "pdftoolkit/version.hpp"

namespace {

using pdftoolkit::ErrorCode;

PDTK_TEST(cpp_error_codes_match_the_c_abi_codes) {
    // The FFI layer translates with a plain static_cast; any drift here
    // would corrupt status reporting across the boundary.
    PDTK_ASSERT_EQ(static_cast<int32_t>(ErrorCode::Ok), PDTK_OK);
    PDTK_ASSERT_EQ(static_cast<int32_t>(ErrorCode::InvalidArgument), PDTK_ERR_INVALID_ARGUMENT);
    PDTK_ASSERT_EQ(static_cast<int32_t>(ErrorCode::BufferTooSmall), PDTK_ERR_BUFFER_TOO_SMALL);
    PDTK_ASSERT_EQ(static_cast<int32_t>(ErrorCode::IoTruncated), PDTK_ERR_IO_TRUNCATED);
    PDTK_ASSERT_EQ(static_cast<int32_t>(ErrorCode::OutputConflict), PDTK_ERR_OUTPUT_CONFLICT);
    PDTK_ASSERT_EQ(static_cast<int32_t>(ErrorCode::UnreadablePdf), PDTK_ERR_UNREADABLE_PDF);
    PDTK_ASSERT_EQ(static_cast<int32_t>(ErrorCode::DocumentNotFound), PDTK_ERR_DOCUMENT_NOT_FOUND);
    PDTK_ASSERT_EQ(static_cast<int32_t>(ErrorCode::ContentNotFound), PDTK_ERR_CONTENT_NOT_FOUND);
    PDTK_ASSERT_EQ(static_cast<int32_t>(ErrorCode::InvalidPageRange), PDTK_ERR_INVALID_PAGE_RANGE);
    PDTK_ASSERT_EQ(static_cast<int32_t>(ErrorCode::NotImplemented), PDTK_ERR_NOT_IMPLEMENTED);
    PDTK_ASSERT_EQ(static_cast<int32_t>(ErrorCode::Internal), PDTK_ERR_INTERNAL);
}

PDTK_TEST(error_message_is_never_null_and_never_empty) {
    const int32_t codes[] = {
        PDTK_OK,                     PDTK_ERR_INVALID_ARGUMENT, PDTK_ERR_BUFFER_TOO_SMALL,
        PDTK_ERR_IO_TRUNCATED,       PDTK_ERR_OUTPUT_CONFLICT,   PDTK_ERR_UNREADABLE_PDF,
        PDTK_ERR_DOCUMENT_NOT_FOUND, PDTK_ERR_CONTENT_NOT_FOUND, PDTK_ERR_INVALID_PAGE_RANGE,
        PDTK_ERR_NOT_IMPLEMENTED,    PDTK_ERR_INTERNAL,
    };
    for (const int32_t code : codes) {
        const auto error_code = static_cast<ErrorCode>(code);
        const char* message = pdftoolkit::error_message(error_code);
        PDTK_ASSERT(message != nullptr);
        PDTK_ASSERT(std::strlen(message) > 0);
    }
    // Unknown codes still yield a safe fallback.
    PDTK_ASSERT(pdftoolkit::error_message(static_cast<ErrorCode>(9999)) != nullptr);
}

PDTK_TEST(known_codes_have_distinct_messages) {
    // Distinctness matters for log forensics; only the not-implemented
    // family may share wording with nothing else.
    const char* ok = pdftoolkit::error_message(ErrorCode::Ok);
    const char* invalid = pdftoolkit::error_message(ErrorCode::InvalidArgument);
    const char* truncated = pdftoolkit::error_message(ErrorCode::IoTruncated);
    const char* conflict = pdftoolkit::error_message(ErrorCode::OutputConflict);
    const char* unreadable = pdftoolkit::error_message(ErrorCode::UnreadablePdf);
    const char* not_found = pdftoolkit::error_message(ErrorCode::DocumentNotFound);
    PDTK_ASSERT(std::strcmp(ok, invalid) != 0);
    PDTK_ASSERT(std::strcmp(invalid, truncated) != 0);
    PDTK_ASSERT(std::strcmp(truncated, conflict) != 0);
    PDTK_ASSERT(std::strcmp(conflict, unreadable) != 0);
    PDTK_ASSERT(std::strcmp(unreadable, not_found) != 0);
}

PDTK_TEST(version_string_is_stable_and_shaped) {
    const char* version = pdftoolkit::native_version();
    PDTK_ASSERT(version != nullptr);
    PDTK_ASSERT(std::strcmp(version, "0.1.0") == 0);
}

}  // namespace

PDTK_TEST_MAIN()
