// Tiny dependency-free test framework (no gtest on a stock Mac and we do not download anything).
#pragma once

#include <cstdio>
#include <functional>
#include <iostream>
#include <sstream>
#include <type_traits>
#include <string>
#include <vector>

#include "Json.hpp"

namespace aftest {

struct Case {
    std::string suite, name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
inline int& failures() {
    static int f = 0;
    return f;
}

struct Registrar {
    Registrar(const char* suite, const char* name, std::function<void()> fn) {
        registry().push_back({suite, name, std::move(fn)});
    }
};

struct Failure {
    std::string msg;
};

template <class T> concept Streamable = requires(std::ostream& os, const T& v) { os << v; };

template <class T> std::string show(const T& v) {
    std::ostringstream os;
    if constexpr (std::is_enum_v<T>) os << static_cast<long long>(v);
    else if constexpr (Streamable<T>) os << v;
    else if constexpr (requires { v.begin(); v.end(); }) {
        os << "{";
        bool first = true;
        for (const auto& e : v) { os << (first ? "" : ", ") << show(e); first = false; }
        os << "}";
    } else os << "<?>";
    return os.str();
}

}  // namespace aftest

#define AF_TEST(suite, name)                                                         \
    static void suite##_##name();                                                    \
    static ::aftest::Registrar reg_##suite##_##name(#suite, #name, suite##_##name);  \
    static void suite##_##name()

#define CHECK(cond)                                                                                     \
    do {                                                                                                \
        if (!(cond)) throw ::aftest::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": CHECK(" #cond ")"}; \
    } while (0)

#define CHECK_EQ(a, b)                                                                                   \
    do {                                                                                                 \
        const auto va_ = (a);  /* by value: `a` may be a member of a temporary */                         \
        const auto vb_ = (b);                                                                           \
        if (!(va_ == vb_))                                                                               \
            throw ::aftest::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": CHECK_EQ(" #a ", " #b ") got " + \
                                    ::aftest::show(va_) + " vs " + ::aftest::show(vb_)};                \
    } while (0)

// Passes when `expr` throws FsError with the given errno.
#define CHECK_ERRNO(expr, code)                                                                         \
    do {                                                                                                \
        int got_ = 0;                                                                                   \
        try {                                                                                           \
            (void)(expr);                                                                               \
        } catch (const ::mtport::FsError& e_) {                                                  \
            got_ = e_.err();                                                                            \
        }                                                                                               \
        if (got_ != (code))                                                                             \
            throw ::aftest::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": expected errno " #code " (" + std::to_string(code) + ") from " #expr ", got " + std::to_string(got_)}; \
    } while (0)

// Passes when `expr` throws MtpError with the given code.
#define CHECK_MTP(expr, errc)                                                                           \
    do {                                                                                                \
        bool ok_ = false;                                                                               \
        std::string why_ = "no exception";                                                              \
        try {                                                                                           \
            (void)(expr);                                                                               \
        } catch (const ::mtport::MtpError& e_) {                                                 \
            ok_ = e_.code() == (errc);                                                                  \
            why_ = e_.what();                                                                           \
        } catch (const std::exception& e_) {                                                            \
            why_ = e_.what();                                                                           \
        }                                                                                               \
        if (!ok_) throw ::aftest::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": expected MtpError " #errc " from " #expr ": " + why_}; \
    } while (0)

// Passes when Json::parse(text) throws.
#define CHECK_THROWS_BAD(text)                                                                           \
    do {                                                                                                 \
        bool threw_ = false;                                                                             \
        try { (void)::mtport::Json::parse(text); } catch (const std::exception&) { threw_ = true; } \
        if (!threw_) throw ::aftest::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": expected a parse error for " #text}; \
    } while (0)
