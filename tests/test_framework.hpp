#pragma once

// Tiny zero-dependency test harness.
//
// Deliberately not a third-party framework: this project must build on a bare
// cloud container and on a Windows box with nothing but a compiler and CMake.
//
//   TEST(name) { ... }        registers a test
//   CHECK(cond)               non-fatal assertion
//   CHECK_EQ(a, b)            non-fatal equality assertion
//   REQUIRE(cond)             fatal for the current test
//
// A test binary returns non-zero if any check failed, which is all ctest needs.

#include <concepts>
#include <functional>
#include <iostream>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace azd::test {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failureCount() {
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back(TestCase{name, std::move(fn)});
    }
};

struct FatalFailure {};

inline void reportFailure(const char* file, int line, const std::string& message) {
    ++failureCount();
    std::cout << "    FAIL " << file << ":" << line << "  " << message << "\n";
}

/// Values that can be streamed are printed; anything else (containers, structs)
/// reports its type only, so CHECK_EQ works with any comparable type.
template <typename T>
concept Streamable = requires(std::ostream& os, const T& value) { os << value; };

template <typename T>
std::string describe(const T& value) {
    if constexpr (Streamable<T>) {
        std::ostringstream oss;
        oss << value;
        return oss.str();
    } else {
        (void)value;
        return "<non-printable value>";
    }
}

inline std::string describe(const std::string& value) { return value; }
inline std::string describe(bool value) { return value ? "true" : "false"; }

inline int runAll(const char* suiteName) {
    std::cout << "[ suite ] " << suiteName << "\n";
    int failedTests = 0;
    for (const TestCase& test : registry()) {
        const int before = failureCount();
        std::cout << "  [ test ] " << test.name << "\n";
        try {
            test.fn();
        } catch (const FatalFailure&) {
            // Already reported.
        } catch (const std::exception& e) {
            reportFailure(__FILE__, __LINE__, std::string("unexpected exception: ") + e.what());
        }
        if (failureCount() != before) ++failedTests;
    }
    if (failureCount() == 0) {
        std::cout << "[ pass  ] " << registry().size() << " test(s)\n";
        return 0;
    }
    std::cout << "[ FAIL  ] " << failedTests << " of " << registry().size()
              << " test(s), " << failureCount() << " failed check(s)\n";
    return 1;
}

} // namespace azd::test

#define AZD_CONCAT_INNER(a, b) a##b
#define AZD_CONCAT(a, b) AZD_CONCAT_INNER(a, b)

#define TEST(name)                                                                 \
    static void AZD_CONCAT(azd_test_fn_, __LINE__)();                              \
    static ::azd::test::Registrar AZD_CONCAT(azd_test_reg_, __LINE__)(             \
        name, AZD_CONCAT(azd_test_fn_, __LINE__));                                 \
    static void AZD_CONCAT(azd_test_fn_, __LINE__)()

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            ::azd::test::reportFailure(__FILE__, __LINE__, "CHECK(" #cond ")");     \
        }                                                                          \
    } while (false)

#define CHECK_EQ(a, b)                                                             \
    do {                                                                           \
        const auto& azd_lhs = (a);                                                 \
        const auto& azd_rhs = (b);                                                 \
        if (!(azd_lhs == azd_rhs)) {                                               \
            ::azd::test::reportFailure(                                            \
                __FILE__, __LINE__,                                                \
                std::string("CHECK_EQ(" #a ", " #b ")\n           actual:   ") +   \
                    ::azd::test::describe(azd_lhs) + "\n           expected: " +   \
                    ::azd::test::describe(azd_rhs));                               \
        }                                                                          \
    } while (false)

#define REQUIRE(cond)                                                              \
    do {                                                                           \
        if (!(cond)) {                                                             \
            ::azd::test::reportFailure(__FILE__, __LINE__, "REQUIRE(" #cond ")");   \
            throw ::azd::test::FatalFailure{};                                     \
        }                                                                          \
    } while (false)

#define AZD_TEST_MAIN(suiteName)                                                   \
    int main() { return ::azd::test::runAll(suiteName); }
