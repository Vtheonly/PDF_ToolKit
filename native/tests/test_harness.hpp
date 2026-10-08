// Minimal assert-based test harness for the native Phase-0 suite.
//
// THROWAWAY BY DESIGN (ADR-0003): superseded by GoogleTest when the
// benchmarking pipeline lands (audit task 0.2). Do not grow it.

#pragma once

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

namespace pdtk_test {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> instances;
    return instances;
}

inline int run_all() {
    int failed = 0;
    for (const TestCase& test : registry()) {
        try {
            test.fn();
            std::printf("[ PASS ] %s\n", test.name);
        } catch (const std::exception& error) {
            ++failed;
            std::printf("[ FAIL ] %s: %s\n", test.name, error.what());
        } catch (...) {
            ++failed;
            std::printf("[ FAIL ] %s: unknown exception\n", test.name);
        }
    }
    std::printf("%zu test(s), %d failed\n", registry().size(), failed);
    return failed == 0 ? 0 : 1;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

}  // namespace pdtk_test

#define PDTK_TEST(name)                                                      \
    static void pdtk_test_##name();                                          \
    static const ::pdtk_test::Registrar pdtk_registrar_##name(#name,         \
                                                              &pdtk_test_##name); \
    static void pdtk_test_##name()

#define PDTK_ASSERT(condition)                                               \
    do {                                                                     \
        if (!(condition)) {                                                  \
            throw std::runtime_error(                                        \
                std::string(__FILE__) + ":" + std::to_string(__LINE__) +     \
                " assertion failed: " #condition);                           \
        }                                                                    \
    } while (false)

#define PDTK_ASSERT_EQ(actual, expected)                                     \
    do {                                                                     \
        if (!((actual) == (expected))) {                                     \
            throw std::runtime_error(                                        \
                std::string(__FILE__) + ":" + std::to_string(__LINE__) +     \
                " expected " #actual " == " #expected);                      \
        }                                                                    \
    } while (false)

#define PDTK_TEST_MAIN()                                                     \
    int main() { return ::pdtk_test::run_all(); }
