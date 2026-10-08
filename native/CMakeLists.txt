cmake_minimum_required(VERSION 3.25)
project(pdftoolkit_native VERSION 0.1.0 LANGUAGES C CXX
        DESCRIPTION "pdftoolkit native C++20 core (issue #1)")

# ---------------------------------------------------------------------------
# Language / standard policy (audit issue-1/task-0.1)
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (single-config generators)" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(PDTK_WERROR              "Treat compiler warnings as errors (zero-warning policy)" ON)
option(PDTK_ENABLE_NATIVE_ARCH  "Compile optimized builds with -march=native (see ADR/U-003)" ON)
option(PDTK_BUILD_TESTS         "Build the native test suite" ON)

# pdftoolkit_core is consumed by the shared FFI library (and the Python
# module); it must therefore be position-independent. This became
# mandatory with task 1.1: the SIGBUS guard's thread-local recovery
# pointer needs a TLS relocation model that is legal inside a shared
# object (initial-exec/local-exec in a non-PIC archive does not link).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(cmake/CompilerWarnings.cmake)

if(PDTK_ENABLE_NATIVE_ARCH AND NOT MSVC)
  # Audit task 0.1 requests -march=native for optimized builds. Kept behind
  # an option so distributable builds can turn it off (unknown U-003).
  add_library(pdtk_native_arch INTERFACE)
  target_compile_options(pdtk_native_arch INTERFACE
    $<$<CONFIG:Release>:-march=native>
    $<$<CONFIG:RelWithDebInfo>:-march=native>)
endif()

# ---------------------------------------------------------------------------
# pdftoolkit_core - static systems library
# ---------------------------------------------------------------------------
add_library(pdftoolkit_core STATIC
  src/core/version.cpp
  src/core/errors.cpp
  src/memory/arena.cpp
  src/memory/mmap.cpp
  src/memory/page_slab.cpp
  src/parser/trailer.cpp
  src/parser/xref.cpp
  src/parser/lexer.cpp
)
target_include_directories(pdftoolkit_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(pdftoolkit_core PUBLIC cxx_std_20)
target_link_libraries(pdftoolkit_core PRIVATE pdtk_warnings)
# Pure-C-ABI hygiene: core symbols are hidden; only PDTK_API-annotated
# functions leave the shared FFI library (verified via nm in tests).
set_target_properties(pdftoolkit_core PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)
if(TARGET pdtk_native_arch)
  target_link_libraries(pdftoolkit_core PRIVATE pdtk_native_arch)
endif()

# ---------------------------------------------------------------------------
# pdftoolkit_ffi - shared library exporting the pure C-ABI
# ---------------------------------------------------------------------------
add_library(pdftoolkit_ffi SHARED
  src/ffi/pdftoolkit.cpp
)
target_compile_features(pdftoolkit_ffi PRIVATE cxx_std_20)
# PUBLIC: consumers (tests, extension, CLI) need the core headers too.
target_link_libraries(pdftoolkit_ffi PUBLIC pdftoolkit_core PRIVATE pdtk_warnings)
set_target_properties(pdftoolkit_ffi PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)
if(TARGET pdtk_native_arch)
  target_link_libraries(pdftoolkit_ffi PRIVATE pdtk_native_arch)
endif()

# ---------------------------------------------------------------------------
# pdftoolkit_py - Python native extension (raw C-API, ADR-0003)
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter Development.Module REQUIRED)
add_library(pdftoolkit_py MODULE
  src/python/pdftoolkit_native.cpp
)
target_compile_features(pdftoolkit_py PRIVATE cxx_std_20)
target_link_libraries(pdftoolkit_py PRIVATE pdftoolkit_ffi pdtk_warnings Python3::Module)
set_target_properties(pdftoolkit_py PROPERTIES
  PREFIX ""
  OUTPUT_NAME "pdftoolkit_native"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)

# ---------------------------------------------------------------------------
# pdftoolkit_cli - native compiled executable
# ---------------------------------------------------------------------------
add_executable(pdftoolkit_cli
  src/cli/main.cpp
)
target_compile_features(pdftoolkit_cli PRIVATE cxx_std_20)
target_link_libraries(pdftoolkit_cli PRIVATE pdftoolkit_ffi pdtk_warnings)
if(TARGET pdtk_native_arch)
  target_link_libraries(pdftoolkit_cli PRIVATE pdtk_native_arch)
endif()

# ---------------------------------------------------------------------------
# Tests (minimal in-repo harness, ADR-0003; GoogleTest migration deferred —
# see ADR-0005 and unknowns U-009. Do not grow the harness.)
# ---------------------------------------------------------------------------
if(PDTK_BUILD_TESTS)
  add_executable(pdtk_test_arena tests/test_arena.cpp)
  target_link_libraries(pdtk_test_arena PRIVATE pdftoolkit_core pdtk_warnings)

  add_executable(pdtk_test_errors tests/test_errors.cpp)
  target_link_libraries(pdtk_test_errors PRIVATE pdftoolkit_core pdtk_warnings)

  add_executable(pdtk_test_mmap tests/test_mmap.cpp)
  target_link_libraries(pdtk_test_mmap PRIVATE pdftoolkit_core pdtk_warnings)

  add_executable(pdtk_test_slab tests/test_slab.cpp)
  target_link_libraries(pdtk_test_slab PRIVATE pdftoolkit_core pdtk_warnings)

  add_executable(pdtk_test_trailer tests/test_trailer.cpp)
  target_link_libraries(pdtk_test_trailer PRIVATE pdftoolkit_core pdtk_warnings)

  add_executable(pdtk_test_xref tests/test_xref.cpp)
  target_link_libraries(pdtk_test_xref PRIVATE pdftoolkit_core pdtk_warnings)

  add_executable(pdtk_test_lexer tests/test_lexer.cpp)
  target_link_libraries(pdtk_test_lexer PRIVATE pdftoolkit_core pdtk_warnings)

  add_executable(pdtk_test_ffi tests/test_ffi.cpp)
  target_link_libraries(pdtk_test_ffi PRIVATE pdftoolkit_ffi pdtk_warnings)

  # Pure-C compilation smoke test: proves pdftoolkit.h is a valid C header
  # and the C-ABI is consumable from C (direction of audit task 7.1).
  add_executable(pdtk_c_abi_smoke tests/c_abi_smoke.c)
  target_link_libraries(pdtk_c_abi_smoke PRIVATE pdftoolkit_ffi pdtk_warnings)

  add_test(NAME arena        COMMAND pdtk_test_arena)
  add_test(NAME errors       COMMAND pdtk_test_errors)
  add_test(NAME mmap         COMMAND pdtk_test_mmap)
  add_test(NAME slab         COMMAND pdtk_test_slab)
  add_test(NAME trailer      COMMAND pdtk_test_trailer)
  add_test(NAME xref         COMMAND pdtk_test_xref)
  add_test(NAME lexer        COMMAND pdtk_test_lexer)
  add_test(NAME ffi          COMMAND pdtk_test_ffi)
  add_test(NAME c_abi_smoke  COMMAND pdtk_c_abi_smoke)
  add_test(NAME cli_version  COMMAND pdftoolkit_cli --version)

  add_test(NAME python_import_smoke
    COMMAND ${CMAKE_COMMAND} -E env
            "PYTHONPATH=$<TARGET_FILE_DIR:pdftoolkit_py>"
            ${Python3_EXECUTABLE} -c
            "import pdftoolkit_native as m; v = m.native_version(); assert isinstance(v, str) and v; assert m.error_message(0) == 'ok'; assert m.engine_smoke(); print('python import ok:', v)")
endif()

# ---------------------------------------------------------------------------
# Benchmarks (audit issue-1/task-0.2; policy: ADR-0005, resolves U-006)
# ---------------------------------------------------------------------------
option(PDTK_ENABLE_BENCHMARKS
       "Fetch Google Benchmark at configure time (needs network) and build the benchmark suite; turn OFF for offline builds (ADR-0005)" ON)
if(PDTK_ENABLE_BENCHMARKS)
  include(FetchContent)
  FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.5  # pinned release; bump deliberately, never float
  )
  # Third-party project: keep it out of our warning profile and testing;
  # only the benchmark TUs we own are held to the zero-warning policy.
  set(BENCHMARK_ENABLE_TESTING OFF CACHE INTERNAL "google/benchmark: no self-tests")
  set(BENCHMARK_ENABLE_INSTALL  OFF CACHE INTERNAL "google/benchmark: no install")
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE INTERNAL "google/benchmark: no gtest")
  FetchContent_MakeAvailable(benchmark)
  add_subdirectory(benchmarks)
endif()
