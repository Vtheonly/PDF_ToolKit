// C-ABI behaviour tests for pdftoolkit-ffi (Phase 0 scaffolding).

#include "test_harness.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "pdftoolkit/pdftoolkit.h"

namespace {

PDTK_TEST(engine_create_rejects_null_out_pointer) {
    PDTK_ASSERT_EQ(pdftoolkit_engine_create(nullptr), PDTK_ERR_INVALID_ARGUMENT);
}

PDTK_TEST(engine_lifecycle_create_destroy) {
    EngineHandle* engine = nullptr;
    PDTK_ASSERT_EQ(pdftoolkit_engine_create(&engine), PDTK_OK);
    PDTK_ASSERT(engine != nullptr);
    PDTK_ASSERT_EQ(pdftoolkit_engine_destroy(engine), PDTK_OK);
    // Destroying null is an explicit error (lifetime bugs must surface).
    PDTK_ASSERT_EQ(pdftoolkit_engine_destroy(nullptr), PDTK_ERR_INVALID_ARGUMENT);
}

PDTK_TEST(version_rejects_bad_buffers) {
    char buffer[64];
    PDTK_ASSERT_EQ(pdftoolkit_version(nullptr, sizeof(buffer)), PDTK_ERR_INVALID_ARGUMENT);
    PDTK_ASSERT_EQ(pdftoolkit_version(buffer, 0), PDTK_ERR_INVALID_ARGUMENT);
    // "0.1.0" + NUL = 6 bytes: 5 is too small, 6 fits exactly.
    char tiny[5];
    PDTK_ASSERT_EQ(pdftoolkit_version(tiny, 5), PDTK_ERR_BUFFER_TOO_SMALL);
}

PDTK_TEST(version_returns_the_core_version_string) {
    char buffer[64];
    PDTK_ASSERT_EQ(pdftoolkit_version(buffer, static_cast<uint32_t>(sizeof(buffer))), PDTK_OK);
    PDTK_ASSERT_EQ(std::string(buffer), std::string("0.1.0"));
}

class TempFile {
public:
    explicit TempFile(const char* name)
        : path_(std::filesystem::temp_directory_path() / name) {
        std::ofstream out(path_, std::ios::binary);
        out << "phase-0 placeholder (not yet parsed as PDF)";
    }
    ~TempFile() { std::error_code ec; std::filesystem::remove(path_, ec); }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

PDTK_TEST(register_document_validates_arguments) {
    EngineHandle* engine = nullptr;
    PDTK_ASSERT_EQ(pdftoolkit_engine_create(&engine), PDTK_OK);
    uint32_t doc_id = 0;
    PDTK_ASSERT_EQ(pdftoolkit_register_document(nullptr, "x.pdf", &doc_id),
                   PDTK_ERR_INVALID_ARGUMENT);
    PDTK_ASSERT_EQ(pdftoolkit_register_document(engine, nullptr, &doc_id),
                   PDTK_ERR_INVALID_ARGUMENT);
    PDTK_ASSERT_EQ(pdftoolkit_register_document(engine, "x.pdf", nullptr),
                   PDTK_ERR_INVALID_ARGUMENT);
    PDTK_ASSERT_EQ(pdftoolkit_engine_destroy(engine), PDTK_OK);
}

PDTK_TEST(register_document_reports_missing_files) {
    EngineHandle* engine = nullptr;
    PDTK_ASSERT_EQ(pdftoolkit_engine_create(&engine), PDTK_OK);
    uint32_t doc_id = 0;
    PDTK_ASSERT_EQ(
        pdftoolkit_register_document(engine, "/nonexistent/definitely-missing.pdf", &doc_id),
        PDTK_ERR_DOCUMENT_NOT_FOUND);
    PDTK_ASSERT_EQ(pdftoolkit_engine_destroy(engine), PDTK_OK);
}

PDTK_TEST(register_document_assigns_stable_ids) {
    TempFile file("pdtk_ffi_test_a.tmp");
    EngineHandle* engine = nullptr;
    PDTK_ASSERT_EQ(pdftoolkit_engine_create(&engine), PDTK_OK);

    uint32_t first = 0;
    uint32_t again = 0;
    uint32_t other = 0;
    PDTK_ASSERT_EQ(
        pdftoolkit_register_document(engine, file.path().string().c_str(), &first), PDTK_OK);
    PDTK_ASSERT_EQ(
        pdftoolkit_register_document(engine, file.path().string().c_str(), &again), PDTK_OK);
    PDTK_ASSERT_EQ(first, again);  // same path -> same stable id

    {
        TempFile second("pdtk_ffi_test_b.tmp");
        PDTK_ASSERT_EQ(
            pdftoolkit_register_document(engine, second.path().string().c_str(), &other), PDTK_OK);
        PDTK_ASSERT(first != other);  // distinct paths -> distinct ids
    }
    PDTK_ASSERT_EQ(pdftoolkit_engine_destroy(engine), PDTK_OK);
}

PDTK_TEST(search_wand_is_an_honest_stub) {
    EngineHandle* engine = nullptr;
    PDTK_ASSERT_EQ(pdftoolkit_engine_create(&engine), PDTK_OK);

    FfiResultSlice slice{};
    PDTK_ASSERT_EQ(pdftoolkit_search_wand(engine, "query", 10, 1000, &slice),
                   PDTK_ERR_NOT_IMPLEMENTED);
    PDTK_ASSERT(slice.data == nullptr);
    PDTK_ASSERT_EQ(slice.length, 0u);

    PDTK_ASSERT_EQ(pdftoolkit_search_wand(nullptr, "query", 10, 1000, &slice),
                   PDTK_ERR_INVALID_ARGUMENT);
    PDTK_ASSERT_EQ(pdftoolkit_search_wand(engine, nullptr, 10, 1000, &slice),
                   PDTK_ERR_INVALID_ARGUMENT);
    PDTK_ASSERT_EQ(pdftoolkit_search_wand(engine, "query", 10, 1000, nullptr),
                   PDTK_ERR_INVALID_ARGUMENT);

    pdftoolkit_result_slice_free(nullptr);  // null-safe, must not crash
    pdftoolkit_result_slice_free(&slice);
    PDTK_ASSERT_EQ(pdftoolkit_engine_destroy(engine), PDTK_OK);
}

}  // namespace

PDTK_TEST_MAIN()
