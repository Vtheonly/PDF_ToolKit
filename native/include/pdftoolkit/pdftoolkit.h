/* pdftoolkit.h — the pure C-ABI of the pdftoolkit native engine.
 *
 * Consumable from C, C++ and any foreign runtime (Python, Go, Zig, ...).
 * Contract (audit issue #1, task 7.1):
 *
 *   1. No C++ exception ever crosses this boundary: every entry point is
 *      declared PDTK_NOEXCEPT and wraps its body in try/catch(...),
 *      mapping failures to the integer status codes below.
 *   2. All functions return PDTK_OK (0) on success and a PDTK_ERR_* code
 *      on failure; output parameters are only written on success unless
 *      documented otherwise.
 *   3. Memory ownership: engine handles belong to the caller after
 *      *_create and must be released with *_destroy. Result slices point
 *      at engine-owned memory and must be released with
 *      pdftoolkit_result_slice_free.
 *
 * Phase-0 scaffolding status (honest stubs — see docs/recovery/):
 *   - engine create/destroy and version are fully implemented;
 *   - pdftoolkit_register_document validates arguments and file
 *     existence and assigns stable ids, but does NOT yet parse or mmap
 *     the document (arrives with task 1.1) — any existing file is
 *     accepted at this stage;
 *   - pdftoolkit_search_wand returns PDTK_ERR_NOT_IMPLEMENTED (the WAND
 *     engine arrives with task 4.2).
 */

#ifndef PDFTOOLKIT_FFI_H
#define PDFTOOLKIT_FFI_H

#include <stdint.h>

#ifdef __cplusplus
#define PDTK_NOEXCEPT noexcept
#else
#define PDTK_NOEXCEPT
#endif

/* Export annotation: keeps the C entry points visible even when the
 * library is compiled with -fvisibility=hidden (pure-C-ABI hygiene:
 * only these symbols may leave the shared object). */
#if defined(_WIN32)
#define PDTK_API
#elif defined(__GNUC__)
#define PDTK_API __attribute__((visibility("default")))
#else
#define PDTK_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes. Values are stable ABI; append only, never renumber. */
enum {
    PDTK_OK = 0,
    PDTK_ERR_INVALID_ARGUMENT = 1,
    PDTK_ERR_BUFFER_TOO_SMALL = 2,
    PDTK_ERR_IO_TRUNCATED = 3,
    PDTK_ERR_OUTPUT_CONFLICT = 4,
    PDTK_ERR_UNREADABLE_PDF = 5,
    PDTK_ERR_DOCUMENT_NOT_FOUND = 6,
    PDTK_ERR_CONTENT_NOT_FOUND = 7,
    PDTK_ERR_INVALID_PAGE_RANGE = 8,
    PDTK_ERR_NOT_IMPLEMENTED = 9,
    PDTK_ERR_INTERNAL = 10,
    PDTK__STATUS_COUNT = 11
};

/* Opaque engine instance. */
typedef struct EngineHandle EngineHandle;

/* One ranked search result (page-level). */
typedef struct FfiResultEntry {
    uint32_t doc_id;
    uint32_t page_index;
    float score;
} FfiResultEntry;

/* Borrowed view of engine-owned result memory. */
typedef struct FfiResultSlice {
    const FfiResultEntry* data;
    uint32_t length;
} FfiResultSlice;

/* Create an engine. *out_handle must be released with
 * pdftoolkit_engine_destroy. Null out_handle is an invalid argument. */
PDTK_API int32_t pdftoolkit_engine_create(EngineHandle** out_handle) PDTK_NOEXCEPT;

/* Destroy an engine created by pdftoolkit_engine_create. Null handle is
 * reported as an invalid argument (not a silent no-op) so lifetime bugs
 * surface early. */
PDTK_API int32_t pdftoolkit_engine_destroy(EngineHandle* handle) PDTK_NOEXCEPT;

/* Copy the NUL-terminated native-core version string into out_buf.
 * Returns PDTK_ERR_BUFFER_TOO_SMALL when buf_len cannot hold the string
 * plus its terminator. */
PDTK_API int32_t pdftoolkit_version(char* out_buf, uint32_t buf_len) PDTK_NOEXCEPT;

/* Register a document with the engine, assigning it a stable id.
 * Scaffolding note: validates argument and file existence only —
 * actual mmap/parse arrives with audit task 1.1. */
PDTK_API int32_t pdftoolkit_register_document(EngineHandle* handle,
                                              const char* path,
                                              uint32_t* out_doc_id) PDTK_NOEXCEPT;

/* Execute a Block-Max WAND query (audit task 4.2). NOT IMPLEMENTED in
 * Phase 0: returns PDTK_ERR_NOT_IMPLEMENTED and leaves *out_slice
 * zero-initialised. */
PDTK_API int32_t pdftoolkit_search_wand(EngineHandle* handle,
                                         const char* query,
                                         uint32_t top_k,
                                         uint64_t timeout_ms,
                                         FfiResultSlice* out_slice) PDTK_NOEXCEPT;

/* Release a result slice obtained from the engine. Null-safe. */
PDTK_API void pdftoolkit_result_slice_free(FfiResultSlice* slice) PDTK_NOEXCEPT;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PDFTOOLKIT_FFI_H */
