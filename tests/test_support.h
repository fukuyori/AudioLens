#pragma once

// Minimal test harness. AudioLens deliberately carries no third-party
// dependencies, and the DSP/buffer code under test needs little more than
// assertions and a pass/fail tally.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace altest {

struct TestCase {
    const char* name;
    void (*fn)();
};

std::vector<TestCase>& registry();

struct Registrar {
    Registrar(const char* name, void (*fn)());
};

void reportFailure(const char* file, int line, const std::string& message);

int runAll();

template <typename T>
std::string describe(const T& value) {
    if constexpr (std::is_convertible_v<T, std::string>) {
        return std::string(value);
    } else if constexpr (std::is_floating_point_v<T>) {
        return std::to_string(static_cast<double>(value));
    } else if constexpr (std::is_integral_v<T>) {
        return std::to_string(static_cast<long long>(value));
    } else {
        return "<value>";
    }
}

}  // namespace altest

#define AL_TEST(name)                                             \
    static void name();                                           \
    static ::altest::Registrar al_reg_##name(#name, &name);       \
    static void name()

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ::altest::reportFailure(__FILE__, __LINE__, "CHECK failed: " #cond); \
        }                                                                       \
    } while (false)

#define CHECK_EQ(actual, expected)                                                    \
    do {                                                                              \
        const auto al_a = (actual);                                                   \
        const auto al_b = (expected);                                                 \
        if (!(al_a == al_b)) {                                                        \
            ::altest::reportFailure(__FILE__, __LINE__,                               \
                                    std::string(#actual " == " #expected " : got ") + \
                                        ::altest::describe(al_a) + ", expected " +    \
                                        ::altest::describe(al_b));                    \
        }                                                                             \
    } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                       \
    do {                                                                              \
        const double al_a = static_cast<double>(actual);                              \
        const double al_b = static_cast<double>(expected);                            \
        if (!(std::fabs(al_a - al_b) <= (tolerance))) {                               \
            ::altest::reportFailure(__FILE__, __LINE__,                               \
                                    std::string(#actual " ~= " #expected " : got ") + \
                                        std::to_string(al_a) + ", expected " +        \
                                        std::to_string(al_b));                        \
        }                                                                             \
    } while (false)
