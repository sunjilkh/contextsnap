// microtest — a ~150 line test framework with zero dependencies.
//
// ContextSnap deliberately avoids pulling GoogleTest/Catch2 into the build:
// contributors can compile and run a single test file with one g++ invocation,
// and CI stays fast on all three platforms.
//
//   #include "framework/microtest.hpp"
//
//   TEST("urls are sanitised") {
//       CHECK_EQ(sanitize("a?token=1"), "a");
//   }
//
//   MICROTEST_MAIN()
//
// Run all tests:            ./test_sanitizer
// Run a subset (substring): ./test_sanitizer urls
#pragma once

#include <cmath>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace microtest {

struct AssertionFailure : std::exception {
    std::string message;

    explicit AssertionFailure(std::string text) : message(std::move(text)) {}

    [[nodiscard]] const char* what() const noexcept override { return message.c_str(); }
};

struct TestCase {
    std::string name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> body) {
        registry().push_back(TestCase{std::move(name), std::move(body)});
    }
};

/// Stringifies anything streamable; falls back to a placeholder otherwise.
template <typename T>
std::string describe(const T& value) {
    std::ostringstream out;
    if constexpr (std::is_same_v<T, bool>) {
        out << (value ? "true" : "false");
    } else if constexpr (std::is_enum_v<T>) {
        out << static_cast<long long>(value);
    } else {
        out << value;
    }
    return out.str();
}

inline std::string describe(std::nullptr_t) { return "nullptr"; }

[[noreturn]] inline void fail(const std::string& message, const char* file, int line) {
    std::ostringstream out;
    out << file << ":" << line << ": " << message;
    throw AssertionFailure(out.str());
}

inline int run_all(int argc, char** argv) {
    const std::string filter = argc > 1 ? argv[1] : std::string{};
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;

    for (const TestCase& test : registry()) {
        if (!filter.empty() && test.name.find(filter) == std::string::npos) {
            ++skipped;
            continue;
        }
        try {
            test.body();
            std::cout << "  ok   " << test.name << "\n";
            ++passed;
        } catch (const AssertionFailure& failure) {
            std::cout << "  FAIL " << test.name << "\n       " << failure.what() << "\n";
            ++failed;
        } catch (const std::exception& error) {
            std::cout << "  FAIL " << test.name << "\n       unexpected exception: " << error.what()
                      << "\n";
            ++failed;
        } catch (...) {
            std::cout << "  FAIL " << test.name << "\n       unexpected non-standard exception\n";
            ++failed;
        }
    }

    std::cout << (failed == 0 ? "PASS" : "FAIL") << "  " << passed << " passed, " << failed
              << " failed";
    if (skipped > 0) {
        std::cout << ", " << skipped << " filtered out";
    }
    std::cout << "\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace microtest

#define MICROTEST_CONCAT_INNER(a, b) a##b
#define MICROTEST_CONCAT(a, b) MICROTEST_CONCAT_INNER(a, b)

/// Declares a test. The name is a string so it can contain spaces.
#define TEST(test_name)                                                                   \
    static void MICROTEST_CONCAT(microtest_body_, __LINE__)();                            \
    static const ::microtest::Registrar MICROTEST_CONCAT(microtest_registrar_, __LINE__)(  \
        test_name, &MICROTEST_CONCAT(microtest_body_, __LINE__));                          \
    static void MICROTEST_CONCAT(microtest_body_, __LINE__)()

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            ::microtest::fail("expected " #condition, __FILE__, __LINE__);                 \
        }                                                                                 \
    } while (false)

#define CHECK_FALSE(condition) CHECK(!(condition))

#define CHECK_EQ(actual, expected)                                                        \
    do {                                                                                  \
        const auto& microtest_actual = (actual);                                          \
        const auto& microtest_expected = (expected);                                      \
        if (!(microtest_actual == microtest_expected)) {                                  \
            ::microtest::fail(std::string("expected " #actual " == " #expected "\n")       \
                                  .append("         actual:   ")                          \
                                  .append(::microtest::describe(microtest_actual))         \
                                  .append("\n         expected: ")                         \
                                  .append(::microtest::describe(microtest_expected)),      \
                              __FILE__, __LINE__);                                        \
        }                                                                                 \
    } while (false)

#define CHECK_NE(actual, expected)                                                        \
    do {                                                                                  \
        if ((actual) == (expected)) {                                                     \
            ::microtest::fail("expected " #actual " != " #expected, __FILE__, __LINE__);    \
        }                                                                                 \
    } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                           \
    do {                                                                                  \
        const double microtest_delta =                                                    \
            std::fabs(static_cast<double>(actual) - static_cast<double>(expected));       \
        if (microtest_delta > static_cast<double>(tolerance)) {                           \
            ::microtest::fail(std::string("expected " #actual " within " #tolerance        \
                                          " of " #expected ", delta ")                    \
                                  .append(::microtest::describe(microtest_delta)),         \
                              __FILE__, __LINE__);                                        \
        }                                                                                 \
    } while (false)

/// Asserts that a contextsnap Result/Status succeeded, printing the error.
#define CHECK_OK(result_expression)                                                       \
    do {                                                                                  \
        auto&& microtest_result = (result_expression);                                    \
        if (!microtest_result) {                                                          \
            ::microtest::fail(std::string("expected " #result_expression " to succeed: ")  \
                                  .append(microtest_result.error().to_string()),           \
                              __FILE__, __LINE__);                                        \
        }                                                                                 \
    } while (false)

#define CHECK_ERR(result_expression)                                                      \
    do {                                                                                  \
        auto&& microtest_result = (result_expression);                                    \
        if (microtest_result) {                                                           \
            ::microtest::fail("expected " #result_expression " to fail", __FILE__,          \
                              __LINE__);                                                  \
        }                                                                                 \
    } while (false)

#define MICROTEST_MAIN()                                                                  \
    int main(int argc, char** argv) { return ::microtest::run_all(argc, argv); }
