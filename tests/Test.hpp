#pragma once

// Minimal self-contained test harness (no external dependencies).

#include <cstdio>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace fo4mx::test {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

template <class T>
auto printable(const T& v)
{
    if constexpr (std::is_enum_v<T>)
        return static_cast<long long>(v);
    else
        return v;
}

template <class A, class B>
void checkEqual(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line)
{
    if (!(a == b)) {
        std::ostringstream os;
        os << file << ":" << line << ": CHECK_EQ(" << ea << ", " << eb << ") failed: " << printable(a) << " vs " << printable(b);
        throw Failure(os.str());
    }
}

} // namespace fo4mx::test

#define FO4MX_CAT2(a, b) a##b
#define FO4MX_CAT(a, b) FO4MX_CAT2(a, b)
#define TEST(name)                                                                                                     \
    static void FO4MX_CAT(test_, name)();                                                                              \
    static fo4mx::test::Registrar FO4MX_CAT(reg_, name)(#name, FO4MX_CAT(test_, name));                                \
    static void FO4MX_CAT(test_, name)()

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond))                                                                                                   \
            throw fo4mx::test::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": CHECK(" #cond       \
                                                                                                ") failed");           \
    } while (0)

#define CHECK_EQ(a, b) fo4mx::test::checkEqual((a), (b), #a, #b, __FILE__, __LINE__)
